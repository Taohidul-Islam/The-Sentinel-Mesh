/*
 * ============================================================
 * Inferno Sentinel Mesh v3.1 - TRANSMITTER 2 (Tilt + Flood Node)
 * ============================================================
 * Board  : DOIT ESP32 DevKit V1
 * Sensors: MPU6050 (I2C 0x68) - tilt/erosion
 *          HC-SR04 (GPIO 5/18) - water distance/flood
 *
 * Wiring:
 *   MPU6050 SDA -> GPIO 21 | SCL -> GPIO 22
 *   MPU6050 VCC -> 3.3V | GND -> GND | AD0 -> GND
 *   HC-SR04 VCC -> 3.3V
 *   HC-SR04 TRIG -> GPIO 5
 *   HC-SR04 ECHO -> GPIO 18 (direct, no divider)
 *   Reset pin    -> GPIO 13 (hold to GND on boot 3s = clear WiFi)
 *   Status LED   -> GPIO 2
 *
 * WiFi behaviour:
 *   - Saved credentials -> STA mode, DHCP, AP off
 *   - No credentials or fail -> AP mode at 192.168.4.1
 *   - GPIO 13 held 3s on boot -> clears WiFi -> AP mode
 *
 * Flood alert fix:
 *   Baseline is set ONCE on first valid sonar reading or manually.
 *   Alert fires when (baseline - current distance) >= threshold.
 *   Water rising = distance decreasing = positive delta = flood.
 *   Baseline must be greater than zero to prevent false triggers.
 *
 * v3.1.0
 * ============================================================
 */

#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <WebServer.h>
#include <Preferences.h>
#include <MPU6050.h>

#define FW_VERSION  "3.1.0"
#define I2C_SDA     21
#define I2C_SCL     22
#define SONAR_TRIG   5
#define SONAR_ECHO  18
#define STATUS_LED   2
#define RESET_PIN   13

const char* AP_SSID = "Inferno-TX2";
const char* AP_PASS = "inferno123";

uint8_t RECEIVER_MAC[6] = { 0x30, 0xAE, 0xA4, 0xF2, 0x6E, 0xC0 };

#define ESPNOW_CHANNEL    1
#define SEND_INTERVAL_MS  8000UL
#define DEFAULT_TILT_ALERT_DEG  2.0f
#define DEFAULT_FLOOD_DROP_CM  15.0f
// minimum valid sonar reading - readings below this are ignored
#define SONAR_MIN_CM    2.0f
#define SONAR_MAX_CM  400.0f

// ============================================================
// PACKET STRUCT
// ============================================================
typedef struct __attribute__((packed)) {
  uint8_t  nodeId;
  float    temp;
  float    humidity;
  float    pressure;
  float    tiltDeg;
  float    distanceCm;
  bool     alertRain;
  bool     alertStorm;
  bool     alertErosion;
  bool     alertFlood;
  uint8_t  staIP[4];
} SensorPacket;

// ============================================================
// GLOBALS
// ============================================================
Preferences prefs;
WebServer   server(80);
MPU6050     mpu;

bool mpuOk   = false;
bool staMode = false;

float curTiltDeg    = 0;
float curDistanceCm = 0;

float tiltZeroPitch   = 0;
float tiltZeroRoll    = 0;
float tiltAlertDeg    = DEFAULT_TILT_ALERT_DEG;
float sonarBaselineCm = 0;     // 0 = not set yet
float floodDropThresh = DEFAULT_FLOOD_DROP_CM;

bool alertErosion = false;
bool alertFlood   = false;

char cfgSSID[64] = "";
char cfgPASS[64] = "";

unsigned long lastSendMs  = 0;
unsigned long lastBlinkMs = 0;
bool ledState = false;

// ============================================================
// NVS
// ============================================================
void loadConfig() {
  prefs.begin("inf_tx2", false);
  tiltZeroPitch  = prefs.getFloat("tZeroPitch", 0.0f);
  tiltZeroRoll   = prefs.getFloat("tZeroRoll",  0.0f);
  tiltAlertDeg   = prefs.getFloat("tAlert",     DEFAULT_TILT_ALERT_DEG);
  sonarBaselineCm = prefs.getFloat("sonarBase",  0.0f);
  floodDropThresh = prefs.getFloat("floodDrop",  DEFAULT_FLOOD_DROP_CM);
  prefs.getString("ssid", cfgSSID, sizeof(cfgSSID));
  prefs.getString("pass", cfgPASS, sizeof(cfgPASS));
  prefs.end();
  Serial.printf("[CONFIG] baseline=%.1f floodDrop=%.1f tiltAlert=%.1f\n",
                sonarBaselineCm, floodDropThresh, tiltAlertDeg);
}

void saveFloat(const char* key, float val) {
  prefs.begin("inf_tx2", false); prefs.putFloat(key, val); prefs.end();
}
void saveStr(const char* key, const char* val) {
  prefs.begin("inf_tx2", false); prefs.putString(key, val); prefs.end();
}
void clearWiFiConfig() {
  prefs.begin("inf_tx2", false); prefs.remove("ssid"); prefs.remove("pass"); prefs.end();
  Serial.println("[RESET] WiFi cleared");
}

// ============================================================
// RESET PIN
// ============================================================
void checkResetPin() {
  pinMode(RESET_PIN, INPUT_PULLUP);
  if (digitalRead(RESET_PIN) == LOW) {
    Serial.println("[RESET] Held - waiting 3s...");
    unsigned long t = millis();
    while (millis() - t < 3000) {
      if (digitalRead(RESET_PIN) != LOW) return;
      delay(100);
    }
    clearWiFiConfig();
    delay(500);
    ESP.restart();
  }
}

// ============================================================
// WIFI
// ============================================================
void initWiFi() {
  if (strlen(cfgSSID) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfgSSID, cfgPASS);
    Serial.print("[WIFI] Connecting to "); Serial.print(cfgSSID);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 12000) {
      delay(300); Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      staMode = true;
      Serial.print("\n[WIFI] Connected. IP: "); Serial.println(WiFi.localIP());
      return;
    }
    Serial.println("\n[WIFI] STA failed - falling back to AP");
  }
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, ESPNOW_CHANNEL);
  Serial.print("[WIFI] AP started. IP: "); Serial.println(WiFi.softAPIP());
}

// ============================================================
// ESP-NOW
// ============================================================
void onDataSent(const wifi_tx_info_t* info, esp_now_send_status_t status) {
  Serial.printf("[ESPNOW] %s\n", status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

void initEspNow() {
  if (esp_now_init() != ESP_OK) { Serial.println("[ESPNOW] init failed"); return; }
  esp_now_register_send_cb(onDataSent);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, RECEIVER_MAC, 6);
  peer.channel = 0;
  peer.encrypt  = false;
  esp_now_add_peer(&peer);
  Serial.println("[ESPNOW] Ready");
}

// ============================================================
// SENSORS
// ============================================================
void initSensors() {
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  mpu.initialize();
  if (mpu.testConnection()) { mpuOk = true; Serial.println("[SENSOR] MPU6050 OK"); }
  else                       { Serial.println("[SENSOR] MPU6050 NOT FOUND"); }
  pinMode(SONAR_TRIG, OUTPUT);
  pinMode(SONAR_ECHO, INPUT);
  digitalWrite(SONAR_TRIG, LOW);
}

float readTilt() {
  if (!mpuOk) return 0;
  int16_t ax, ay, az;
  mpu.getAcceleration(&ax, &ay, &az);
  float axg = ax/16384.0f, ayg = ay/16384.0f, azg = az/16384.0f;
  float pitch = atan2(ayg, sqrt(axg*axg+azg*azg)) * 180.0f / PI;
  float roll  = atan2(-axg, azg) * 180.0f / PI;
  float pr = pitch - tiltZeroPitch;
  float rr = roll  - tiltZeroRoll;
  return sqrt(pr*pr + rr*rr);
}

float readDistance() {
  digitalWrite(SONAR_TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(SONAR_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(SONAR_TRIG, LOW);
  long dur = pulseIn(SONAR_ECHO, HIGH, 30000);
  if (dur == 0) return curDistanceCm; // no echo - keep last good value
  float d = (dur * 0.0343f) / 2.0f;
  // reject out-of-range readings
  if (d < SONAR_MIN_CM || d > SONAR_MAX_CM) return curDistanceCm;
  return d;
}

void evaluateAlerts() {
  alertErosion = (curTiltDeg >= tiltAlertDeg);

  // flood logic:
  // baseline must be set and valid (>0)
  // water rising = sonar distance decreasing
  // alert when (baseline - current) >= threshold
  // i.e. water has risen by at least floodDropThresh cm toward the sensor
  if (sonarBaselineCm > 0 && curDistanceCm > 0) {
    float rise = sonarBaselineCm - curDistanceCm;
    alertFlood = (rise >= floodDropThresh);
    if (alertFlood) {
      Serial.printf("[ALERT] Flood! baseline=%.1f current=%.1f rise=%.1f thresh=%.1f\n",
                    sonarBaselineCm, curDistanceCm, rise, floodDropThresh);
    }
  } else {
    alertFlood = false;
  }
}

// ============================================================
// ESP-NOW SEND
// ============================================================
void sendPacket() {
  SensorPacket pkt = {};
  pkt.nodeId        = 2;
  pkt.tiltDeg       = curTiltDeg;
  pkt.distanceCm    = curDistanceCm;
  pkt.alertErosion   = alertErosion;
  pkt.alertFlood     = alertFlood;
  if (staMode) {
    IPAddress ip = WiFi.localIP();
    pkt.staIP[0]=ip[0]; pkt.staIP[1]=ip[1]; pkt.staIP[2]=ip[2]; pkt.staIP[3]=ip[3];
  }
  esp_now_send(RECEIVER_MAC, (uint8_t*)&pkt, sizeof(pkt));
  Serial.printf("[TX2] tilt=%.2f dist=%.1f baseline=%.1f | erosion=%d flood=%d\n",
                curTiltDeg, curDistanceCm, sonarBaselineCm, alertErosion, alertFlood);
}

// ============================================================
// WEB SERVER
// ============================================================
String pageHeader(const char* title, bool autoRefresh) {
  String h = "<!DOCTYPE html><html><head>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  if (autoRefresh) h += "<meta http-equiv='refresh' content='8'>";
  h += "<title>"; h += title; h += "</title>";
  h += "<style>body{font-family:sans-serif;margin:20px;background:#111;color:#eee}";
  h += "h2{color:#e05c2a}.card{background:#1c1c1c;padding:14px;border-radius:8px;margin-bottom:12px}";
  h += "input{width:100%;padding:6px;margin:4px 0;box-sizing:border-box;background:#2a2a2a;color:#eee;border:1px solid #444;border-radius:4px}";
  h += "button{background:#8b2000;color:#fff;border:none;padding:8px 16px;border-radius:6px;margin-top:6px;cursor:pointer}";
  h += "a{color:#e08060}label{font-size:12px;color:#999}";
  h += ".ok{color:#6fcf6f}.bad{color:#f07070}</style></head><body>";
  return h;
}

String dashPage() {
  String h = pageHeader("Inferno TX2", true);
  h += "<h2>Inferno TX2 v" FW_VERSION " - Tilt and Flood Node</h2>";

  h += "<div class='card'><b>Live readings</b><br>";
  h += "Tilt         : " + String(curTiltDeg,2) + " deg";
  h += alertErosion ? " <span class='bad'>[EROSION]</span>" : " <span class='ok'>[OK]</span>";
  h += "<br>Water dist   : " + String(curDistanceCm,1) + " cm";
  h += alertFlood ? " <span class='bad'>[FLOOD COMING]</span>" : " <span class='ok'>[OK]</span>";
  h += "<br>Baseline     : " + (sonarBaselineCm > 0 ? String(sonarBaselineCm,1) + " cm" : String("Not set"));
  h += "<br>Water rise   : ";
  if (sonarBaselineCm > 0 && curDistanceCm > 0)
    h += String(sonarBaselineCm - curDistanceCm, 1) + " cm (threshold: " + String(floodDropThresh,1) + " cm)";
  else h += "N/A";
  h += "<br>Network      : ";
  h += staMode ? ("<span class='ok'>STA " + WiFi.localIP().toString() + "</span>")
               : "<span class='bad'>AP 192.168.4.1</span>";
  h += "</div>";

  h += "<div class='card'><b>Settings</b><br>";
  h += "<a href='/sensors'>Sensor config</a><br>";
  h += "<a href='/wifi'>WiFi config</a></div>";
  h += "</body></html>";
  return h;
}

String sensorsPage() {
  String h = pageHeader("TX2 Sensors", false);
  h += "<h2>Inferno TX2 - Sensor config</h2>";

  h += "<div class='card'><b>Tilt calibration</b><br>";
  h += "Current tilt: " + String(curTiltDeg,2) + " deg<br>";
  h += "<form action='/calibrate' method='GET'>";
  h += "<label>Lay device flat then press calibrate to set zero reference</label><br>";
  h += "<button type='submit'>Calibrate flat now</button></form><br>";
  h += "<form action='/setTiltAlert' method='GET'>";
  h += "<label>Tilt alert threshold (deg)</label>";
  h += "<input name='v' type='number' step='0.1' value='" + String(tiltAlertDeg,1) + "'>";
  h += "<button type='submit'>Save</button></form></div>";

  h += "<div class='card'><b>Flood and sonar</b><br>";
  h += "Current distance: " + String(curDistanceCm,1) + " cm<br>";
  h += "Current baseline: " + (sonarBaselineCm > 0 ? String(sonarBaselineCm,1) + " cm" : String("Not set")) + "<br>";
  h += "<form action='/setBaseline' method='GET'>";
  h += "<label>Set baseline to current distance (do this when water is at normal level)</label><br>";
  h += "<button type='submit'>Set baseline now</button></form><br>";
  h += "<form action='/setFloodThresh' method='GET'>";
  h += "<label>Flood alert threshold - water rise in cm from baseline</label>";
  h += "<input name='v' type='number' step='0.1' value='" + String(floodDropThresh,1) + "'>";
  h += "<button type='submit'>Save</button></form></div>";

  h += "<a href='/'>Back</a></body></html>";
  return h;
}

String wifiPage() {
  String h = pageHeader("TX2 WiFi", false);
  h += "<h2>Inferno TX2 - WiFi config</h2>";
  h += "<div class='card'>Current: ";
  h += staMode ? ("<span class='ok'>Connected - " + WiFi.localIP().toString() + "</span>")
               : "<span class='bad'>AP mode</span>";
  h += "<br><br><form action='/saveWifi' method='GET'>";
  h += "<label>WiFi SSID</label><input name='ssid' value='" + String(cfgSSID) + "'>";
  h += "<label>Password</label><input name='pass' type='password'>";
  h += "<button type='submit'>Save and restart</button></form></div>";
  h += "<div class='card'><form action='/resetWifi' method='GET'>";
  h += "<button type='submit'>Clear WiFi and restart in AP mode</button></form>";
  h += "<br>Or hold GPIO 13 to GND on power-up for 3s.</div>";
  h += "<a href='/'>Back</a></body></html>";
  return h;
}

void handleRoot()     { server.send(200, "text/html", dashPage()); }
void handleSensors()  { server.send(200, "text/html", sensorsPage()); }
void handleWifi()     { server.send(200, "text/html", wifiPage()); }

void handleCalibrate() {
  if (mpuOk) {
    int16_t ax, ay, az;
    mpu.getAcceleration(&ax, &ay, &az);
    float axg=ax/16384.0f, ayg=ay/16384.0f, azg=az/16384.0f;
    tiltZeroPitch = atan2(ayg, sqrt(axg*axg+azg*azg)) * 180.0f / PI;
    tiltZeroRoll  = atan2(-axg, azg) * 180.0f / PI;
    saveFloat("tZeroPitch", tiltZeroPitch);
    saveFloat("tZeroRoll",  tiltZeroRoll);
    Serial.printf("[CAL] zero pitch=%.2f roll=%.2f\n", tiltZeroPitch, tiltZeroRoll);
  }
  server.sendHeader("Location", "/sensors"); server.send(303);
}
void handleSetTiltAlert() {
  if (server.hasArg("v")) { tiltAlertDeg = server.arg("v").toFloat(); saveFloat("tAlert", tiltAlertDeg); }
  server.sendHeader("Location", "/sensors"); server.send(303);
}
void handleSetBaseline() {
  if (curDistanceCm > SONAR_MIN_CM) {
    sonarBaselineCm = curDistanceCm;
    saveFloat("sonarBase", sonarBaselineCm);
    Serial.printf("[CAL] Sonar baseline set to %.1f cm\n", sonarBaselineCm);
  } else {
    Serial.println("[CAL] Baseline rejected - invalid sonar reading");
  }
  server.sendHeader("Location", "/sensors"); server.send(303);
}
void handleSetFloodThresh() {
  if (server.hasArg("v")) { floodDropThresh = server.arg("v").toFloat(); saveFloat("floodDrop", floodDropThresh); }
  server.sendHeader("Location", "/sensors"); server.send(303);
}
void handleSaveWifi() {
  if (server.hasArg("ssid")) { server.arg("ssid").toCharArray(cfgSSID, sizeof(cfgSSID)); saveStr("ssid", cfgSSID); }
  if (server.hasArg("pass") && server.arg("pass").length() > 0) {
    server.arg("pass").toCharArray(cfgPASS, sizeof(cfgPASS)); saveStr("pass", cfgPASS);
  }
  server.send(200, "text/html", "<html><body style='background:#111;color:#eee;font-family:sans-serif;padding:20px'><h3>Saved. Restarting...</h3></body></html>");
  delay(1500); ESP.restart();
}
void handleResetWifi() {
  clearWiFiConfig();
  server.send(200, "text/html", "<html><body style='background:#111;color:#eee;font-family:sans-serif;padding:20px'><h3>WiFi cleared. Restarting in AP mode...</h3></body></html>");
  delay(1500); ESP.restart();
}

void initWebServer() {
  server.on("/",              handleRoot);
  server.on("/sensors",       handleSensors);
  server.on("/wifi",          handleWifi);
  server.on("/calibrate",     handleCalibrate);
  server.on("/setTiltAlert",  handleSetTiltAlert);
  server.on("/setBaseline",   handleSetBaseline);
  server.on("/setFloodThresh",handleSetFloodThresh);
  server.on("/saveWifi",      handleSaveWifi);
  server.on("/resetWifi",     handleResetWifi);
  server.begin();
  Serial.println("[WEB] TX2 server started");
}

// ============================================================
// SETUP / LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[BOOT] Inferno Sentinel TX2 v" FW_VERSION);
  pinMode(STATUS_LED, OUTPUT);
  checkResetPin();
  loadConfig();
  initSensors();
  initWiFi();
  initEspNow();
  initWebServer();
  Serial.println("[BOOT] TX2 ready\n");
}

void loop() {
  server.handleClient();
  if (millis() - lastBlinkMs >= 1000) {
    lastBlinkMs = millis(); ledState = !ledState;
    digitalWrite(STATUS_LED, ledState);
  }
  curTiltDeg    = readTilt();
  curDistanceCm = readDistance();
  evaluateAlerts();
  if (millis() - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = millis(); sendPacket();
  }
  delay(200);
}
