/*
 * ============================================================
 * Inferno Sentinel Mesh v3.1 - TRANSMITTER 1 (Weather Node)
 * ============================================================
 * Board  : DOIT ESP32 DevKit V1
 * Sensor : BME280 (I2C) - temperature, humidity, pressure
 *
 * Wiring:
 *   BME280 SCL -> GPIO 21
 *   BME280 SDA -> GPIO 22
 *   BME280 VCC -> 3.3V | GND -> GND
 *   Reset pin  -> GPIO 13 (hold to GND on boot 3s = clear WiFi)
 *   Status LED -> GPIO 2
 *
 * WiFi behaviour:
 *   - If credentials saved: connect to router/hotspot via DHCP
 *     AP mode turns OFF after successful STA connection
 *     Web admin accessible at DHCP-assigned IP
 *   - If no credentials or connection fails: AP mode at 192.168.4.1
 *   - GPIO 13 held to GND on boot for 3s: clears saved credentials
 *
 * v3.1.0
 * ============================================================
 */

#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

#define FW_VERSION  "3.1.0"
#define I2C_SCL     21
#define I2C_SDA     22
#define STATUS_LED   2
#define RESET_PIN   13

const char* AP_SSID = "Inferno-TX1";
const char* AP_PASS = "inferno123";

uint8_t RECEIVER_MAC[6] = { 0x30, 0xAE, 0xA4, 0xF2, 0x6E, 0xC0 };

#define ESPNOW_CHANNEL    1
#define SEND_INTERVAL_MS  8000UL
#define ALERT_BUZZ_MS     10000UL
#define RATE_WINDOW_MS    (10UL * 60UL * 1000UL)
#define DEFAULT_HUM_RATE_THRESH   8.0f
#define DEFAULT_PRES_RATE_THRESH  3.0f

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
Preferences     prefs;
WebServer       server(80);
Adafruit_BME280 bme;

bool bmeOk      = false;
bool staMode    = false; // true = connected to router, AP off

float curTemp     = 0;
float curHumidity = 0;
float curPressure = 0;

float humHistory[2]  = {-1, -1};
float presHistory[2] = {-1, -1};
unsigned long lastRateSampleMs = 0;

bool alertRain  = false;
bool alertStorm = false;

unsigned long rainBuzzStartMs  = 0;
unsigned long stormBuzzStartMs = 0;
bool rainSuppressed  = false;
bool stormSuppressed = false;

float humRateThresh  = DEFAULT_HUM_RATE_THRESH;
float presRateThresh = DEFAULT_PRES_RATE_THRESH;

char cfgSSID[64] = "";
char cfgPASS[64] = "";

unsigned long lastSendMs  = 0;
unsigned long lastBlinkMs = 0;
bool ledState = false;

// ============================================================
// NVS
// ============================================================
void loadConfig() {
  prefs.begin("inf_tx1", false);
  humRateThresh  = prefs.getFloat("humRate",  DEFAULT_HUM_RATE_THRESH);
  presRateThresh = prefs.getFloat("presRate", DEFAULT_PRES_RATE_THRESH);
  prefs.getString("ssid", cfgSSID, sizeof(cfgSSID));
  prefs.getString("pass", cfgPASS, sizeof(cfgPASS));
  prefs.end();
}

void saveFloat(const char* key, float val) {
  prefs.begin("inf_tx1", false); prefs.putFloat(key, val); prefs.end();
}
void saveStr(const char* key, const char* val) {
  prefs.begin("inf_tx1", false); prefs.putString(key, val); prefs.end();
}
void clearWiFiConfig() {
  prefs.begin("inf_tx1", false);
  prefs.remove("ssid"); prefs.remove("pass");
  prefs.end();
  Serial.println("[RESET] WiFi credentials cleared");
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
    // try STA connection
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
      // AP is off in STA-only mode
      return;
    }
    Serial.println("\n[WIFI] STA failed - falling back to AP");
  }

  // fallback: AP mode
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, ESPNOW_CHANNEL);
  Serial.print("[WIFI] AP started. IP: "); Serial.println(WiFi.softAPIP());
}

// ============================================================
// ESP-NOW
// NOTE: ESP-NOW needs WiFi to be up. In STA mode it works fine.
// In AP mode it also works. Channel must match receiver.
// ============================================================
void onDataSent(const wifi_tx_info_t* info, esp_now_send_status_t status) {
  Serial.printf("[ESPNOW] %s\n", status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

void initEspNow() {
  if (esp_now_init() != ESP_OK) { Serial.println("[ESPNOW] init failed"); return; }
  esp_now_register_send_cb(onDataSent);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, RECEIVER_MAC, 6);
  peer.channel = 0; // 0 = use current channel
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
  if (bme.begin(0x76))      { bmeOk = true; Serial.println("[SENSOR] BME280 OK at 0x76"); }
  else if (bme.begin(0x77)) { bmeOk = true; Serial.println("[SENSOR] BME280 OK at 0x77"); }
  else                       { Serial.println("[SENSOR] BME280 NOT FOUND"); }
}

void readBME() {
  if (!bmeOk) return;
  float t = bme.readTemperature();
  float h = bme.readHumidity();
  float p = bme.readPressure() / 100.0f;
  if (t > -40 && t < 85)   curTemp     = t;
  if (h >= 0  && h <= 100) curHumidity = h;
  if (p > 300 && p < 1200) curPressure = p;
}

void updateRateHistory() {
  unsigned long now = millis();
  if (humHistory[1] < 0) {
    humHistory[0] = humHistory[1] = curHumidity;
    presHistory[0] = presHistory[1] = curPressure;
    lastRateSampleMs = now;
    return;
  }
  if (now - lastRateSampleMs >= RATE_WINDOW_MS) {
    humHistory[0] = humHistory[1]; presHistory[0] = presHistory[1];
    humHistory[1] = curHumidity;   presHistory[1] = curPressure;
    lastRateSampleMs = now;
  } else {
    humHistory[1] = curHumidity; presHistory[1] = curPressure;
  }
}

void evaluateAlerts() {
  unsigned long now = millis();
  float humDelta  = humHistory[1]  - humHistory[0];
  float presDelta = presHistory[1] - presHistory[0];
  bool rawRain  = (humDelta  >=  humRateThresh);
  bool rawStorm = (presDelta <= -presRateThresh);

  if (rawRain && !rainSuppressed) {
    if (!alertRain) { alertRain = true; rainBuzzStartMs = now; }
    if (now - rainBuzzStartMs >= ALERT_BUZZ_MS) { rainSuppressed = true; alertRain = false; }
  } else if (!rawRain) { alertRain = false; rainSuppressed = false; rainBuzzStartMs = 0; }

  if (rawStorm && !stormSuppressed) {
    if (!alertStorm) { alertStorm = true; stormBuzzStartMs = now; }
    if (now - stormBuzzStartMs >= ALERT_BUZZ_MS) { stormSuppressed = true; alertStorm = false; }
  } else if (!rawStorm) { alertStorm = false; stormSuppressed = false; stormBuzzStartMs = 0; }
}

// ============================================================
// ESP-NOW SEND
// ============================================================
void sendPacket() {
  SensorPacket pkt = {};
  pkt.nodeId     = 1;
  pkt.temp       = curTemp;
  pkt.humidity   = curHumidity;
  pkt.pressure   = curPressure;
  pkt.alertRain  = alertRain;
  pkt.alertStorm = alertStorm;
  if (staMode) {
    IPAddress ip = WiFi.localIP();
    pkt.staIP[0]=ip[0]; pkt.staIP[1]=ip[1]; pkt.staIP[2]=ip[2]; pkt.staIP[3]=ip[3];
  }
  esp_now_send(RECEIVER_MAC, (uint8_t*)&pkt, sizeof(pkt));
  Serial.printf("[TX1] temp=%.1f hum=%.1f pres=%.1f | rain=%d storm=%d\n",
                curTemp, curHumidity, curPressure, alertRain, alertStorm);
}

// ============================================================
// WEB SERVER
// HTML pages that have input fields do NOT auto-refresh
// Only the root dashboard auto-refreshes
// ============================================================
String pageHeader(const char* title, bool autoRefresh) {
  String h = "<!DOCTYPE html><html><head>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  if (autoRefresh) h += "<meta http-equiv='refresh' content='8'>";
  h += "<title>"; h += title; h += "</title>";
  h += "<style>body{font-family:sans-serif;margin:20px;background:#111;color:#eee}";
  h += "h2{color:#e05c2a}h3{color:#c04010}";
  h += ".card{background:#1c1c1c;padding:14px;border-radius:8px;margin-bottom:12px}";
  h += "input{width:100%;padding:6px;margin:4px 0;box-sizing:border-box;background:#2a2a2a;color:#eee;border:1px solid #444;border-radius:4px}";
  h += "button{background:#8b2000;color:#fff;border:none;padding:8px 16px;border-radius:6px;margin-top:6px;cursor:pointer}";
  h += "a{color:#e08060}label{font-size:12px;color:#999}";
  h += ".ok{color:#6fcf6f}.bad{color:#f07070}</style></head><body>";
  return h;
}

String dashPage() {
  String h = pageHeader("Inferno TX1", true);
  h += "<h2>Inferno TX1 v" FW_VERSION " - Weather Node</h2>";

  h += "<div class='card'><b>Live readings</b><br>";
  h += "Temperature : " + String(curTemp,1) + " C<br>";
  h += "Humidity    : " + String(curHumidity,1) + " %";
  h += alertRain  ? " <span class='bad'>[RAIN SOON]</span>"  : " <span class='ok'>[OK]</span>";
  h += "<br>Pressure    : " + String(curPressure,1) + " hPa";
  h += alertStorm ? " <span class='bad'>[STORM SOON]</span>" : " <span class='ok'>[OK]</span>";
  h += "<br>Network     : ";
  h += staMode ? ("<span class='ok'>STA " + WiFi.localIP().toString() + "</span>")
               : "<span class='bad'>AP 192.168.4.1</span>";
  h += "</div>";

  h += "<div class='card'><b>Settings</b><br>";
  h += "<a href='/thresholds'>Alert thresholds</a><br>";
  h += "<a href='/wifi'>WiFi config</a></div>";
  h += "</body></html>";
  return h;
}

String threshPage() {
  String h = pageHeader("TX1 Thresholds", false); // no auto-refresh - has inputs
  h += "<h2>Inferno TX1 - Alert thresholds</h2>";
  h += "<div class='card'>";
  h += "<form action='/setHumRate' method='GET'>";
  h += "<label>Humidity rise rate (%/10min) - triggers rain alert</label>";
  h += "<input name='v' type='number' step='0.1' value='" + String(humRateThresh,1) + "'>";
  h += "<button type='submit'>Save</button></form><br>";
  h += "<form action='/setPresRate' method='GET'>";
  h += "<label>Pressure drop rate (hPa/10min) - triggers storm alert</label>";
  h += "<input name='v' type='number' step='0.1' value='" + String(presRateThresh,1) + "'>";
  h += "<button type='submit'>Save</button></form></div>";
  h += "<a href='/'>Back</a></body></html>";
  return h;
}

String wifiPage() {
  String h = pageHeader("TX1 WiFi", false); // no auto-refresh - has inputs
  h += "<h2>Inferno TX1 - WiFi config</h2>";
  h += "<div class='card'>";
  h += "Current: ";
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

void handleRoot()        { server.send(200, "text/html", dashPage()); }
void handleThresholds()  { server.send(200, "text/html", threshPage()); }
void handleWifi()        { server.send(200, "text/html", wifiPage()); }

void handleSetHumRate() {
  if (server.hasArg("v")) { humRateThresh = server.arg("v").toFloat(); saveFloat("humRate", humRateThresh); }
  server.sendHeader("Location", "/thresholds"); server.send(303);
}
void handleSetPresRate() {
  if (server.hasArg("v")) { presRateThresh = server.arg("v").toFloat(); saveFloat("presRate", presRateThresh); }
  server.sendHeader("Location", "/thresholds"); server.send(303);
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
  server.on("/",           handleRoot);
  server.on("/thresholds", handleThresholds);
  server.on("/wifi",       handleWifi);
  server.on("/setHumRate", handleSetHumRate);
  server.on("/setPresRate",handleSetPresRate);
  server.on("/saveWifi",   handleSaveWifi);
  server.on("/resetWifi",  handleResetWifi);
  server.begin();
  Serial.println("[WEB] TX1 server started");
}

// ============================================================
// SETUP / LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[BOOT] Inferno Sentinel TX1 v" FW_VERSION);
  pinMode(STATUS_LED, OUTPUT);
  checkResetPin();
  loadConfig();
  initSensors();
  initWiFi();
  initEspNow();
  initWebServer();
  Serial.println("[BOOT] TX1 ready\n");
}

void loop() {
  server.handleClient();
  if (millis() - lastBlinkMs >= 1000) {
    lastBlinkMs = millis(); ledState = !ledState;
    digitalWrite(STATUS_LED, ledState);
  }
  readBME();
  updateRateHistory();
  evaluateAlerts();
  if (millis() - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = millis(); sendPacket();
  }
  delay(200);
}
