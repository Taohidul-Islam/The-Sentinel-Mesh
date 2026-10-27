/*
 * ============================================================
 * Inferno Sentinel Mesh v3.2 - RECEIVER (Alert + Display Node)
 * ============================================================
 * Board : DOIT ESP32 DevKit V1
 *
 * Wiring:
 *   Buzzer NPN base -> GPIO 22 (via 1k resistor)
 *   LED NPN base    -> GPIO 23 (via 1k resistor)
 *   Button          -> GPIO 19 (other leg to GND, INPUT_PULLUP)
 *   OLED SDA        -> GPIO 26
 *   OLED SCL        -> GPIO 27
 *   Reset pin       -> GPIO 13 (hold to GND on boot 3s = wipe ALL data)
 *   Status LED      -> GPIO 2
 *
 * WiFi:
 *   Tries STA (DHCP) if credentials saved
 *   Falls back to AP at 192.168.4.1 if no credentials or fail
 *   LAN IP shown on OLED at boot and status channel
 *
 * GPIO 13 reset:
 *   Wipes ALL NVS: WiFi creds, OWM key, OWM city, laptop IP
 *
 * OLED channels (button cycles):
 *   0 - TX1 weather    1 - TX2 tilt and water
 *   2 - Sensor alerts  3 - OWM forecast
 *   4 - Laptop forecast  5 - Node IPs and status
 *
 * v3.2.0
 * ============================================================
 */

#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <U8g2lib.h>

#define FW_VERSION   "3.2.0"

#define BUZZER_PIN   22
#define LED_PIN      23
#define BTN_PIN      19
#define OLED_SDA     26
#define OLED_SCL     27
#define RESET_PIN    13
#define STATUS_LED    2

const char* AP_SSID = "Inferno-RX";
const char* AP_PASS = "inferno123";
#define ESPNOW_CHANNEL   1
#define NUM_CHANNELS     6
#define BTN_DEBOUNCE_MS  200

#define OWM_POLL_MS      (10UL * 60UL * 1000UL)
#define LAPTOP_POLL_MS   (60UL * 1000UL)
#define LAPTOP_PORT      8080
#define DATA_TIMEOUT_MS  (30UL * 1000UL)
#define ALERT_BUZZ_MS    10000UL

// ============================================================
// TYPES
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

enum AlertType { ALERT_NONE, ALERT_RAIN, ALERT_EROSION, ALERT_STORM, ALERT_FLOOD };
struct PatternStep { unsigned long onMs; unsigned long offMs; };

// ============================================================
// OLED
// ============================================================
U8G2_SH1106_128X64_NONAME_F_SW_I2C oled(U8G2_R0, OLED_SCL, OLED_SDA, U8X8_PIN_NONE);

// ============================================================
// GLOBALS
// ============================================================
Preferences prefs;
WebServer   server(80);

SensorPacket tx1 = {};
SensorPacket tx2 = {};
unsigned long lastRxTx1Ms = 0;
unsigned long lastRxTx2Ms = 0;
volatile bool newDataFlag  = false;

char owmApiKey[64] = "";
char owmCity[64]   = "";
String owmFlood    = "";
String owmCyclone  = "";
String owmSummary  = "";
unsigned long lastOwmPollMs = 0;

char laptopIP[32] = "";
String lapFlood   = "";
String lapCyclone = "";
String lapSummary = "";
unsigned long lastLapPollMs = 0;

char cfgSSID[64] = "";
char cfgPASS[64] = "";
bool staMode = false;

bool alertRain    = false;
bool alertStorm   = false;
bool alertErosion = false;
bool alertFlood   = false;

unsigned long rainBuzzStartMs  = 0;
unsigned long stormBuzzStartMs = 0;
bool rainSuppressed  = false;
bool stormSuppressed = false;

AlertType currentPattern = ALERT_NONE;
unsigned long patternStartMs = 0;

volatile int  displayChannel = 0;
unsigned long lastBtnMs      = 0;

unsigned long lastBlinkMs = 0;
bool ledState = false;

PatternStep ledErosion[] = { {400, 1600} };
PatternStep ledRain[]    = { {150, 150}, {150, 1550} };
PatternStep ledStorm[]   = { {120, 120}, {120, 120}, {120, 1020} };
PatternStep ledFlood[]   = { {100, 100} };

PatternStep buzErosion[] = { {600, 1800} };
PatternStep buzRain[]    = { {100, 120}, {100, 2080} };
PatternStep buzStorm[]   = { {80, 80}, {80, 80}, {80, 80}, {300, 800} };
PatternStep buzFlood[]   = { {70, 60} };

// ============================================================
// HELPERS
// ============================================================
String ipStr(uint8_t* b) {
  return String(b[0])+"."+String(b[1])+"."+String(b[2])+"."+String(b[3]);
}

// ============================================================
// NVS
// ============================================================
void loadConfig() {
  prefs.begin("inf_rx", false);
  prefs.getString("owmKey",  owmApiKey, sizeof(owmApiKey));
  prefs.getString("owmCity", owmCity,   sizeof(owmCity));
  prefs.getString("lapIP",   laptopIP,  sizeof(laptopIP));
  prefs.getString("ssid",    cfgSSID,   sizeof(cfgSSID));
  prefs.getString("pass",    cfgPASS,   sizeof(cfgPASS));
  prefs.end();
}

void saveStr(const char* key, const char* val) {
  prefs.begin("inf_rx", false); prefs.putString(key, val); prefs.end();
}

void wipeAllNVS() {
  prefs.begin("inf_rx", false);
  prefs.clear();
  prefs.end();
  Serial.println("[RESET] All NVS data wiped");
}

// ============================================================
// RESET PIN
// ============================================================
void checkResetPin() {
  pinMode(RESET_PIN, INPUT_PULLUP);
  if (digitalRead(RESET_PIN) == LOW) {
    Serial.println("[RESET] Held - waiting 3s to confirm...");
    unsigned long t = millis();
    while (millis() - t < 3000) {
      if (digitalRead(RESET_PIN) != LOW) return;
      delay(100);
    }
    Serial.println("[RESET] Confirmed - wiping all data...");
    wipeAllNVS();
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
  Serial.print("[WIFI] AP IP: "); Serial.println(WiFi.softAPIP());
}

// ============================================================
// ESP-NOW
// ============================================================
void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != sizeof(SensorPacket)) return;
  SensorPacket pkt;
  memcpy(&pkt, data, sizeof(pkt));
  if (pkt.nodeId == 1) { memcpy(&tx1, &pkt, sizeof(pkt)); lastRxTx1Ms = millis(); }
  if (pkt.nodeId == 2) { memcpy(&tx2, &pkt, sizeof(pkt)); lastRxTx2Ms = millis(); }
  newDataFlag = true;
}

void initEspNow() {
  if (esp_now_init() != ESP_OK) { Serial.println("[ESPNOW] init failed"); return; }
  esp_now_register_recv_cb(onDataRecv);
  Serial.println("[ESPNOW] Receiver ready");
  Serial.print("[ESPNOW] MAC: "); Serial.println(WiFi.macAddress());
}

// ============================================================
// ALERT LOGIC
// ============================================================
void evaluateAlerts() {
  unsigned long now = millis();
  bool tx1Fresh = (lastRxTx1Ms > 0 && millis() - lastRxTx1Ms < DATA_TIMEOUT_MS);
  bool tx2Fresh = (lastRxTx2Ms > 0 && millis() - lastRxTx2Ms < DATA_TIMEOUT_MS);

  bool rawRain    = tx1Fresh && tx1.alertRain;
  bool rawStorm   = tx1Fresh && tx1.alertStorm;
  bool rawErosion = tx2Fresh && tx2.alertErosion;
  bool rawFlood   = tx2Fresh && tx2.alertFlood;

  alertErosion = rawErosion;
  alertFlood   = rawFlood;

  if (rawRain && !rainSuppressed) {
    if (!alertRain) { alertRain = true; rainBuzzStartMs = now; }
    if (now - rainBuzzStartMs >= ALERT_BUZZ_MS) { rainSuppressed = true; alertRain = false; }
  } else if (!rawRain) { alertRain = false; rainSuppressed = false; rainBuzzStartMs = 0; }

  if (rawStorm && !stormSuppressed) {
    if (!alertStorm) { alertStorm = true; stormBuzzStartMs = now; }
    if (now - stormBuzzStartMs >= ALERT_BUZZ_MS) { stormSuppressed = true; alertStorm = false; }
  } else if (!rawStorm) { alertStorm = false; stormSuppressed = false; stormBuzzStartMs = 0; }
}

AlertType priorityAlert() {
  if (alertFlood)   return ALERT_FLOOD;
  if (alertStorm)   return ALERT_STORM;
  if (alertErosion) return ALERT_EROSION;
  if (alertRain)    return ALERT_RAIN;
  return ALERT_NONE;
}

// ============================================================
// PATTERN ENGINE
// ============================================================
bool patternState(PatternStep* steps, int len, unsigned long startMs) {
  unsigned long total = 0;
  for (int i = 0; i < len; i++) total += steps[i].onMs + steps[i].offMs;
  if (!total) return false;
  unsigned long elapsed = (millis() - startMs) % total;
  unsigned long acc = 0;
  for (int i = 0; i < len; i++) {
    if (elapsed < acc + steps[i].onMs) return true;
    if (elapsed < acc + steps[i].onMs + steps[i].offMs) return false;
    acc += steps[i].onMs + steps[i].offMs;
  }
  return false;
}

void runPattern() {
  AlertType active = priorityAlert();
  if (active != currentPattern) { currentPattern = active; patternStartMs = millis(); }
  if (active == ALERT_NONE) {
    digitalWrite(LED_PIN, LOW); digitalWrite(BUZZER_PIN, LOW); return;
  }
  PatternStep* ls; int ll;
  PatternStep* bs; int bl;
  switch (active) {
    case ALERT_EROSION: ls=ledErosion; ll=1; bs=buzErosion; bl=1; break;
    case ALERT_RAIN:    ls=ledRain;    ll=2; bs=buzRain;    bl=2; break;
    case ALERT_STORM:   ls=ledStorm;   ll=3; bs=buzStorm;   bl=4; break;
    case ALERT_FLOOD:   ls=ledFlood;   ll=1; bs=buzFlood;   bl=1; break;
    default: return;
  }
  digitalWrite(LED_PIN,    patternState(ls, ll, patternStartMs) ? HIGH : LOW);
  digitalWrite(BUZZER_PIN, patternState(bs, bl, patternStartMs) ? HIGH : LOW);
}

// ============================================================
// OWM POLL
// ============================================================
void pollOWM() {
  if (!staMode || strlen(owmApiKey)==0 || strlen(owmCity)==0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = "http://api.openweathermap.org/data/2.5/forecast?q=";
  url += String(owmCity) + "&appid=" + String(owmApiKey) + "&units=metric&cnt=8";
  http.begin(url); http.setTimeout(8000);
  int code = http.GET();

  if (code == 200) {
    String body = http.getString();
    owmFlood = ""; owmCyclone = ""; owmSummary = "";
    String bl = body; bl.toLowerCase();

    if (bl.indexOf("flood")>=0 || bl.indexOf("heavy rain")>=0 ||
        bl.indexOf("torrential")>=0 || bl.indexOf("extreme rain")>=0)
      owmFlood = "Flood or heavy rain risk";

    if (bl.indexOf("cyclone")>=0 || bl.indexOf("tropical storm")>=0 ||
        bl.indexOf("hurricane")>=0 || bl.indexOf("typhoon")>=0 ||
        bl.indexOf("violent storm")>=0)
      owmCyclone = "Severe storm or cyclone risk";

    int idx = body.indexOf("\"description\":\"");
    if (idx >= 0) {
      idx += 15;
      int end = body.indexOf("\"", idx);
      owmSummary = body.substring(idx, end);
      if (owmSummary.length() > 0) owmSummary[0] = toupper(owmSummary[0]);
    }
    if (owmFlood.length()   == 0) owmFlood   = "None";
    if (owmCyclone.length() == 0) owmCyclone = "None";
    Serial.printf("[OWM] flood=%s cyclone=%s\n", owmFlood.c_str(), owmCyclone.c_str());
  } else {
    owmSummary = "Error " + String(code);
    Serial.printf("[OWM] failed code=%d\n", code);
  }
  http.end();
}

// ============================================================
// LAPTOP POLL
// ============================================================
void pollLaptop() {
  if (!staMode || strlen(laptopIP)==0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = "http://" + String(laptopIP) + ":" + String(LAPTOP_PORT) + "/alerts";
  http.begin(url); http.setTimeout(5000);
  int code = http.GET();

  if (code == 200) {
    String body = http.getString();
    lapFlood = ""; lapCyclone = ""; lapSummary = "";
    int idx;
    idx = body.indexOf("flood:");
    if (idx>=0) { int e=body.indexOf('\n',idx); lapFlood   = body.substring(idx+6, e>0?e:body.length()); lapFlood.trim(); }
    idx = body.indexOf("cyclone:");
    if (idx>=0) { int e=body.indexOf('\n',idx); lapCyclone = body.substring(idx+8, e>0?e:body.length()); lapCyclone.trim(); }
    idx = body.indexOf("summary:");
    if (idx>=0) { int e=body.indexOf('\n',idx); lapSummary = body.substring(idx+8, e>0?e:body.length()); lapSummary.trim(); }
    Serial.printf("[LAP] flood=%s cyclone=%s\n", lapFlood.c_str(), lapCyclone.c_str());
  } else {
    Serial.printf("[LAP] failed code=%d\n", code);
  }
  http.end();
}

// ============================================================
// OLED
// ============================================================
void oledLine(const char* text, int x, int& y, int lh) {
  oled.drawStr(x, y, text); y += lh;
}

void updateOLED() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tr);
  int y=10; int lh=11;
  bool tx1Fresh = (lastRxTx1Ms>0 && millis()-lastRxTx1Ms < DATA_TIMEOUT_MS);
  bool tx2Fresh = (lastRxTx2Ms>0 && millis()-lastRxTx2Ms < DATA_TIMEOUT_MS);
  char buf[32];

  switch (displayChannel) {
    case 0:
      oledLine("TX1 WEATHER", 0, y, lh);
      if (!tx1Fresh) { oledLine("No data / stale", 0, y, lh); break; }
      snprintf(buf,sizeof(buf),"Temp : %.1f C",   tx1.temp);     oledLine(buf,0,y,lh);
      snprintf(buf,sizeof(buf),"Hum  : %.1f %%",  tx1.humidity); oledLine(buf,0,y,lh);
      snprintf(buf,sizeof(buf),"Pres : %.1f hPa", tx1.pressure); oledLine(buf,0,y,lh);
      if (tx1.alertRain)  oledLine("RAIN SOON",  0,y,lh);
      if (tx1.alertStorm) oledLine("STORM SOON", 0,y,lh);
      break;

    case 1:
      oledLine("TX2 FIELD", 0, y, lh);
      if (!tx2Fresh) { oledLine("No data / stale", 0, y, lh); break; }
      snprintf(buf,sizeof(buf),"Tilt : %.2f deg",  tx2.tiltDeg);    oledLine(buf,0,y,lh);
      snprintf(buf,sizeof(buf),"Water: %.1f cm",   tx2.distanceCm); oledLine(buf,0,y,lh);
      if (tx2.alertErosion) oledLine("EROSION",      0,y,lh);
      if (tx2.alertFlood)   oledLine("FLOOD COMING", 0,y,lh);
      break;

    case 2:
      oledLine("SENSOR ALERTS", 0, y, lh);
      if (!alertRain && !alertStorm && !alertErosion && !alertFlood)
        { oledLine("All clear", 0, y, lh); break; }
      if (alertFlood)   oledLine("FLOOD COMING", 0,y,lh);
      if (alertStorm)   oledLine("STORM SOON",   0,y,lh);
      if (alertErosion) oledLine("EROSION",      0,y,lh);
      if (alertRain)    oledLine("RAIN SOON",    0,y,lh);
      break;

    case 3:
      oledLine("OWM FORECAST", 0, y, lh);
      if (strlen(owmApiKey)==0) { oledLine("No API key set",  0,y,lh); break; }
      if (!staMode)             { oledLine("No WiFi",          0,y,lh); break; }
      snprintf(buf,sizeof(buf),"FL:%s", owmFlood.length()>0   ? owmFlood.c_str()   : "No data"); oledLine(buf,0,y,lh);
      snprintf(buf,sizeof(buf),"CY:%s", owmCyclone.length()>0 ? owmCyclone.c_str() : "No data"); oledLine(buf,0,y,lh);
      if (owmSummary.length()>0) oledLine(owmSummary.c_str(), 0,y,lh);
      break;

    case 4:
      oledLine("LAPTOP FORECAST", 0, y, lh);
      if (strlen(laptopIP)==0) { oledLine("No laptop IP set", 0,y,lh); break; }
      if (!staMode)            { oledLine("No WiFi",           0,y,lh); break; }
      snprintf(buf,sizeof(buf),"FL:%s", lapFlood.length()>0   ? lapFlood.c_str()   : "No data"); oledLine(buf,0,y,lh);
      snprintf(buf,sizeof(buf),"CY:%s", lapCyclone.length()>0 ? lapCyclone.c_str() : "No data"); oledLine(buf,0,y,lh);
      if (lapSummary.length()>0) oledLine(lapSummary.c_str(), 0,y,lh);
      break;

    case 5:
      oledLine("SYSTEM STATUS", 0, y, lh);
      if (staMode) snprintf(buf,sizeof(buf),"RX: %s", WiFi.localIP().toString().c_str());
      else         snprintf(buf,sizeof(buf),"RX: 192.168.4.1 AP");
      oledLine(buf,0,y,lh);
      if (tx1Fresh && tx1.staIP[0]>0) snprintf(buf,sizeof(buf),"TX1: %s", ipStr(tx1.staIP).c_str());
      else snprintf(buf,sizeof(buf),"TX1: %s", tx1Fresh ? "AP only" : "offline");
      oledLine(buf,0,y,lh);
      if (tx2Fresh && tx2.staIP[0]>0) snprintf(buf,sizeof(buf),"TX2: %s", ipStr(tx2.staIP).c_str());
      else snprintf(buf,sizeof(buf),"TX2: %s", tx2Fresh ? "AP only" : "offline");
      oledLine(buf,0,y,lh);
      break;
  }

  for (int i=0; i<NUM_CHANNELS; i++) {
    if (i==displayChannel) oled.drawBox(38+i*9,61,5,3);
    else                   oled.drawFrame(38+i*9,61,5,3);
  }
  oled.sendBuffer();
}

// ============================================================
// BUTTON
// ============================================================
void checkButton() {
  if (digitalRead(BTN_PIN)==LOW && millis()-lastBtnMs > BTN_DEBOUNCE_MS) {
    lastBtnMs = millis();
    displayChannel = (displayChannel+1) % NUM_CHANNELS;
    updateOLED();
  }
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
  h += ".ok{color:#6fcf6f}.bad{color:#f07070}.dim{color:#888}</style></head><body>";
  return h;
}

String dashPage() {
  bool tx1Fresh = (lastRxTx1Ms>0 && millis()-lastRxTx1Ms < DATA_TIMEOUT_MS);
  bool tx2Fresh = (lastRxTx2Ms>0 && millis()-lastRxTx2Ms < DATA_TIMEOUT_MS);

  String h = pageHeader("Inferno RX", true);
  h += "<h2>Inferno Sentinel RX v" FW_VERSION "</h2>";

  h += "<div class='card'><b>Active alerts</b><br>";
  if (!alertRain && !alertStorm && !alertErosion && !alertFlood)
    h += "<span class='ok'>All clear</span>";
  if (alertFlood)   h += "<span class='bad'>FLOOD COMING </span>";
  if (alertStorm)   h += "<span class='bad'>STORM SOON </span>";
  if (alertErosion) h += "<span class='bad'>EROSION </span>";
  if (alertRain)    h += "<span class='bad'>RAIN SOON </span>";
  h += "</div>";

  h += "<div class='card'><b>TX1 Weather</b> ";
  h += tx1Fresh ? "<span class='ok'>[live]</span>" : "<span class='bad'>[stale]</span>";
  h += "<br>Temp: "+String(tx1.temp,1)+" C  Hum: "+String(tx1.humidity,1)+"%  Pres: "+String(tx1.pressure,1)+" hPa";
  if (tx1.staIP[0]>0) h += "<br>TX1 IP: " + ipStr(tx1.staIP);
  h += "</div>";

  h += "<div class='card'><b>TX2 Field</b> ";
  h += tx2Fresh ? "<span class='ok'>[live]</span>" : "<span class='bad'>[stale]</span>";
  h += "<br>Tilt: "+String(tx2.tiltDeg,2)+" deg  Water: "+String(tx2.distanceCm,1)+" cm";
  if (tx2.staIP[0]>0) h += "<br>TX2 IP: " + ipStr(tx2.staIP);
  h += "</div>";

  h += "<div class='card'><b>OWM forecast</b><br>";
  h += "Flood: "  +(owmFlood.length()>0   ? owmFlood   : String("No data"))+"<br>";
  h += "Cyclone: "+(owmCyclone.length()>0 ? owmCyclone : String("No data"))+"<br>";
  h += "Summary: "+(owmSummary.length()>0 ? owmSummary : String("No data"))+"</div>";

  h += "<div class='card'><b>Laptop forecast</b><br>";
  h += "Flood: "  +(lapFlood.length()>0   ? lapFlood   : String("No data"))+"<br>";
  h += "Cyclone: "+(lapCyclone.length()>0 ? lapCyclone : String("No data"))+"<br>";
  h += "Summary: "+(lapSummary.length()>0 ? lapSummary : String("No data"))+"</div>";

  h += "<div class='card'><b>Network</b><br>RX: ";
  h += staMode ? ("<span class='ok'>"+WiFi.localIP().toString()+"</span>")
               : "<span class='bad'>AP only - 192.168.4.1</span>";
  h += "</div>";

  h += "<div class='card'><b>Settings</b><br>";
  h += "<a href='/weather'>Weather API config</a><br>";
  h += "<a href='/wifi'>WiFi config</a><br>";
  h += "<a href='/reset'>Factory reset</a></div>";
  h += "</body></html>";
  return h;
}

String weatherPage() {
  String h = pageHeader("RX Weather", false);
  h += "<h2>Inferno RX - Weather config</h2>";
  h += "<div class='card'><b>OWM direct</b><br>";
  h += "<form action='/saveOwm' method='GET'>";
  h += "<label>OWM API key</label><input name='key' value='" + String(owmApiKey) + "'>";
  h += "<label>City name</label><input name='city' value='" + String(owmCity) + "'>";
  h += "<button type='submit'>Save and poll now</button></form></div>";

  h += "<div class='card'><b>Laptop server</b><br>";
  h += "Saved: <b>" + String(strlen(laptopIP)>0 ? laptopIP : "not set") + "</b><br>";
  h += "<form action='/saveLap' method='GET'>";
  h += "<label>Laptop IP on same network</label>";
  h += "<input name='ip' value='" + String(laptopIP) + "'>";
  h += "<button type='submit'>Save and poll now</button></form></div>";
  h += "<a href='/'>Back</a></body></html>";
  return h;
}

String wifiPage() {
  String h = pageHeader("RX WiFi", false);
  h += "<h2>Inferno RX - WiFi config</h2>";
  h += "<div class='card'>Current: ";
  h += staMode ? ("<span class='ok'>Connected - "+WiFi.localIP().toString()+"</span>")
               : "<span class='bad'>AP mode - 192.168.4.1</span>";
  h += "<br><br><form action='/saveWifi' method='GET'>";
  h += "<label>WiFi SSID</label><input name='ssid' value='" + String(cfgSSID) + "'>";
  h += "<label>Password</label><input name='pass' type='password'>";
  h += "<button type='submit'>Save and restart</button></form></div>";
  h += "<div class='card'><form action='/clearWifi' method='GET'>";
  h += "<button type='submit'>Disconnect WiFi and use AP mode</button></form></div>";
  h += "<a href='/'>Back</a></body></html>";
  return h;
}

String resetPage() {
  String h = pageHeader("RX Reset", false);
  h += "<h2>Inferno RX - Factory reset</h2>";
  h += "<div class='card'><span class='bad'>Wipes ALL saved data:</span><br>";
  h += "WiFi credentials, OWM API key, OWM city, laptop IP<br><br>";
  h += "<form action='/doReset' method='GET'>";
  h += "<button type='submit'>Wipe all data and restart</button></form></div>";
  h += "Or hold GPIO 13 to GND on power-up for 3s.<br>";
  h += "<a href='/'>Back</a></body></html>";
  return h;
}

void handleRoot()      { server.send(200, "text/html", dashPage()); }
void handleWeather()   { server.send(200, "text/html", weatherPage()); }
void handleWifi()      { server.send(200, "text/html", wifiPage()); }
void handleResetPage() { server.send(200, "text/html", resetPage()); }

void handleSaveOwm() {
  if (server.hasArg("key"))  { server.arg("key").toCharArray(owmApiKey, sizeof(owmApiKey)); saveStr("owmKey",  owmApiKey); }
  if (server.hasArg("city")) { server.arg("city").toCharArray(owmCity,   sizeof(owmCity));   saveStr("owmCity", owmCity);   }
  lastOwmPollMs = 0;
  server.sendHeader("Location", "/weather"); server.send(303);
}
void handleSaveLap() {
  if (server.hasArg("ip")) { server.arg("ip").toCharArray(laptopIP, sizeof(laptopIP)); saveStr("lapIP", laptopIP); }
  lastLapPollMs = 0;
  server.sendHeader("Location", "/weather"); server.send(303);
}
void handleSaveWifi() {
  if (server.hasArg("ssid")) { server.arg("ssid").toCharArray(cfgSSID, sizeof(cfgSSID)); saveStr("ssid", cfgSSID); }
  if (server.hasArg("pass") && server.arg("pass").length()>0) {
    server.arg("pass").toCharArray(cfgPASS, sizeof(cfgPASS)); saveStr("pass", cfgPASS);
  }
  server.send(200, "text/html", "<html><body style='background:#111;color:#eee;font-family:sans-serif;padding:20px'><h3>Saved. Restarting...</h3></body></html>");
  delay(1500); ESP.restart();
}
void handleClearWifi() {
  prefs.begin("inf_rx", false); prefs.remove("ssid"); prefs.remove("pass"); prefs.end();
  cfgSSID[0]=0; cfgPASS[0]=0;
  server.send(200, "text/html", "<html><body style='background:#111;color:#eee;font-family:sans-serif;padding:20px'><h3>WiFi cleared. Restarting...</h3></body></html>");
  delay(1500); ESP.restart();
}
void handleDoReset() {
  wipeAllNVS();
  server.send(200, "text/html", "<html><body style='background:#111;color:#eee;font-family:sans-serif;padding:20px'><h3>All data wiped. Restarting...</h3></body></html>");
  delay(1500); ESP.restart();
}

void initWebServer() {
  server.on("/",          handleRoot);
  server.on("/weather",   handleWeather);
  server.on("/wifi",      handleWifi);
  server.on("/reset",     handleResetPage);
  server.on("/saveOwm",   handleSaveOwm);
  server.on("/saveLap",   handleSaveLap);
  server.on("/saveWifi",  handleSaveWifi);
  server.on("/clearWifi", handleClearWifi);
  server.on("/doReset",   handleDoReset);
  server.begin();
  Serial.println("[WEB] RX server started");
}

// ============================================================
// SETUP / LOOP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[BOOT] Inferno Sentinel RX v" FW_VERSION);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN,    OUTPUT);
  pinMode(STATUS_LED, OUTPUT);
  pinMode(BTN_PIN,    INPUT_PULLUP);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN,    LOW);

  Wire.begin(OLED_SDA, OLED_SCL);
  oled.begin();
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tr);
  oled.drawStr(0, 20, "Inferno Sentinel");
  oled.drawStr(0, 35, "Booting...");
  oled.sendBuffer();

  checkResetPin();
  loadConfig();
  initWiFi();
  initEspNow();
  initWebServer();

  oled.clearBuffer();
  oled.drawStr(0, 15, "Inferno RX Ready");
  if (staMode) {
    oled.drawStr(0, 30, "STA connected:");
    oled.drawStr(0, 43, WiFi.localIP().toString().c_str());
  } else {
    oled.drawStr(0, 30, "AP mode:");
    oled.drawStr(0, 43, "192.168.4.1");
  }
  oled.sendBuffer();
  delay(2500);

  Serial.println("[BOOT] RX ready\n");
}

void loop() {
  server.handleClient();
  checkButton();
  evaluateAlerts();
  runPattern();

  if (millis() - lastBlinkMs >= 1000) {
    lastBlinkMs = millis(); ledState = !ledState;
    digitalWrite(STATUS_LED, ledState);
  }

  if (staMode && strlen(owmApiKey)>0 && millis()-lastOwmPollMs >= OWM_POLL_MS) {
    lastOwmPollMs = millis(); pollOWM();
  }
  if (staMode && strlen(laptopIP)>0 && millis()-lastLapPollMs >= LAPTOP_POLL_MS) {
    lastLapPollMs = millis(); pollLaptop();
  }

  static unsigned long lastOledMs = 0;
  if (millis() - lastOledMs >= 2000) {
    lastOledMs = millis(); updateOLED();
  }

  if (newDataFlag) {
    newDataFlag = false;
    Serial.printf("[RX] TX1:temp=%.1f hum=%.1f pres=%.1f | TX2:tilt=%.2f dist=%.1f\n",
                  tx1.temp, tx1.humidity, tx1.pressure, tx2.tiltDeg, tx2.distanceCm);
  }

  delay(50);
}
