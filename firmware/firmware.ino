#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>

#include <Wire.h>
#include <esp_random.h>
#include <CH224X_I2C.h>

Preferences prefs;

String deviceId;
String bleName;
bool deviceConnected = false;

String computeDeviceId() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[7];
  snprintf(buf, sizeof(buf), "%06X", (unsigned int)(mac & 0xFFFFFF));
  return String(buf);
}

#define SDA_PIN      5
#define SCL_PIN      6
#define PG_PIN       7

#define CH224_ADDR_PRIMARY   0x23
#define CH224_ADDR_SECONDARY 0x22

CH224X_I2C* CH224X1 = nullptr;
uint8_t ch224Addr = CH224_ADDR_PRIMARY;

bool ch224Begin() {
  if (CH224X1 != nullptr) {
    delete CH224X1;
    CH224X1 = nullptr;
  }
  CH224X1 = new CH224X_I2C(Wire, CH224_ADDR_PRIMARY, PG_PIN);
  if (CH224X1->begin()) {
    ch224Addr = CH224_ADDR_PRIMARY;
    return true;
  }
  delete CH224X1;
  CH224X1 = new CH224X_I2C(Wire, CH224_ADDR_SECONDARY, PG_PIN);
  if (CH224X1->begin()) {
    ch224Addr = CH224_ADDR_SECONDARY;
    return true;
  }
  return false;
}

void scanI2CBus() {
  Serial.println("Scanning I2C bus...");
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      Serial.print("  Perangkat I2C ditemukan di alamat 0x");
      if (addr < 16) Serial.print("0");
      Serial.println(addr, HEX);
      found++;
    }
  }
  if (found == 0) {
    Serial.println("  Tidak ada perangkat I2C terdeteksi sama sekali di bus! Cek wiring/pull-up SDA-SCL.");
  } else {
    Serial.print("  Total perangkat I2C ditemukan: ");
    Serial.println(found);
  }
}

#define PIN_LED_DATA 4
#define NUM_LEDS 30
Adafruit_NeoPixel strip(NUM_LEDS, PIN_LED_DATA, NEO_GRB + NEO_KHZ800);
String ledMode = "off";
String lastLedEffect = "running";
unsigned long lastLedStep = 0;
uint16_t rainbowStep = 0;
int bouncePos = 0;
int bounceDir = 1;

#define PIN_ONBOARD_LED 8
#define BOOT_BTN_PIN 9
#define ONBOARD_LED_ACTIVE_LOW true

#define FAN_PWM_PIN   2
#define FAN_TACH_PIN  3
#define FAN_PWM_CHANNEL   0
#define FAN_PWM_FREQ_HZ   25000
#define FAN_PWM_RESOLUTION 8

int fanSpeedPercent = 100;
volatile uint32_t fanTachPulseCount = 0;
unsigned int fanRpm = 0;
unsigned long lastFanRpmCalc = 0;

void IRAM_ATTR fanTachISR() {
  fanTachPulseCount++;
}

void setFanSpeed(int percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  fanSpeedPercent = percent;
  uint8_t duty = (uint8_t)map(percent, 0, 100, 0, 255);
  ledcWrite(FAN_PWM_PIN, duty);

  if (prefs.getInt("fanSpeed", -1) != fanSpeedPercent) {
    prefs.putInt("fanSpeed", fanSpeedPercent);
  }
}

void updateFanRpm() {
  noInterrupts();
  uint32_t pulses = fanTachPulseCount;
  fanTachPulseCount = 0;
  interrupts();

  unsigned long elapsedMs = millis() - lastFanRpmCalc;
  lastFanRpmCalc = millis();
  if (elapsedMs == 0) return;

  fanRpm = (unsigned int)((pulses / 2.0) * (60000.0 / elapsedMs));
}

void onboardLedWrite(bool on) {
  digitalWrite(PIN_ONBOARD_LED, (ONBOARD_LED_ACTIVE_LOW ? !on : on) ? LOW : HIGH);
}

bool cmdBlinkActive = false;
unsigned long cmdBlinkStart = 0;
const unsigned long CMD_BLINK_MS = 150;

unsigned long lastAppContact = 0;
bool statusBlinkOn = false;
unsigned long lastStatusBlinkToggle = 0;
const unsigned long STATUS_BLINK_INTERVAL_MS = 500;
const unsigned long APP_CONTACT_TIMEOUT_MS = 5000;

void triggerCmdBlink() {
  cmdBlinkActive = true;
  cmdBlinkStart = millis();
}

bool isAppConnected() {
  return deviceConnected || (millis() - lastAppContact < APP_CONTACT_TIMEOUT_MS);
}

void handleStatusLed() {
  if (isAppConnected()) {
    if (cmdBlinkActive) {
      if (millis() - cmdBlinkStart < CMD_BLINK_MS) {
        onboardLedWrite(true);
      } else {
        cmdBlinkActive = false;
        onboardLedWrite(false);
      }
    } else {
      onboardLedWrite(false);
    }
  } else {
    if (millis() - lastStatusBlinkToggle >= STATUS_BLINK_INTERVAL_MS) {
      lastStatusBlinkToggle = millis();
      statusBlinkOn = !statusBlinkOn;
    }
    onboardLedWrite(statusBlinkOn);
  }
}

float currentSetVoltage = 5.0;
unsigned long startMillis = 0;
unsigned long lastPublish = 0;
float current = 0;
float chargerwatt = 0;
bool pgood = 0;
bool ch224aReady = false;

String netMode;
String savedSsid;
String savedPass;
bool wifiControlActive = false;
bool configApActive = false;

#define AP_SSID "ESP32-Config"
#define AP_PASS "12345678"

WebServer server(80);

#define HTTP_AUTH_USER "admin01"
String httpAuthPass;

String loadOrCreateHttpAuthPass() {
  String p = prefs.getString("httpAuthPass", "");
  if (p.length() == 0) {
    const char charset[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    char buf[13];
    for (int i = 0; i < 12; i++) {
      uint32_t r = esp_random();
      buf[i] = charset[r % (sizeof(charset) - 1)];
    }
    buf[12] = '\0';
    p = String(buf);
    prefs.putString("httpAuthPass", p);
    Serial.println("Password HTTP unik dibuat untuk device ini.");
  }
  return p;
}

bool checkHttpAuth() {
  if (!server.authenticate(HTTP_AUTH_USER, httpAuthPass.c_str())) {
    server.requestAuthentication();
    return false;
  }
  return true;
}
WiFiUDP udp;
#define UDP_BEACON_PORT 47269
unsigned long lastBeacon = 0;

#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

NimBLEServer* pServer = nullptr;
NimBLECharacteristic* pCharacteristic = nullptr;
volatile bool bleWritePending = false;
portMUX_TYPE bleMux = portMUX_INITIALIZER_UNLOCKED;
char bleCommandBuf[129] = {0};
volatile uint16_t bleCommandLen = 0;

class MyServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    deviceConnected = true;
    Serial.println("BLE: Terhubung ke App!");
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    deviceConnected = false;
    Serial.println("BLE: Terputus, me-restart advertising...");
    NimBLEDevice::getAdvertising()->start();
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    if (connInfo.isEncrypted()) {
      Serial.println("BLE: Koneksi terenkripsi & ter-bonding.");
    } else {
      Serial.println("BLE: Autentikasi gagal / tidak terenkripsi.");
    }
  }
};

class MyCharacteristicCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
    std::string value = characteristic->getValue();
    if (!value.empty()) {
      size_t n = value.size();
      if (n > sizeof(bleCommandBuf) - 1) n = sizeof(bleCommandBuf) - 1;
      portENTER_CRITICAL(&bleMux);
      memcpy(bleCommandBuf, value.data(), n);
      bleCommandBuf[n] = '\0';
      bleCommandLen = (uint16_t)n;
      bleWritePending = true;
      portEXIT_CRITICAL(&bleMux);
    }
  }
};

void applyVoltage(float volt) {
  if (volt >= 13.5) volt = 15.0;
  else if (volt >= 10.5) volt = 12.0;
  else if (volt >= 7.0) volt = 9.0;
  else volt = 5.0;

  if (volt == 9.0) {
    CH224X1->setVoltage(1);
  } else if (volt == 12.0) {
    CH224X1->setVoltage(2);
  } else if (volt == 15.0) {
    CH224X1->setVoltage(3);
  } else {

    CH224X1->setVoltage(0);
  }
  currentSetVoltage = volt;

  if (prefs.getFloat("voltage", -1.0) != currentSetVoltage) {
    prefs.putFloat("voltage", currentSetVoltage);
  }
}

uint32_t wheelColor(byte pos) {
  pos = 255 - pos;
  if (pos < 85) return strip.Color(255 - pos * 3, 0, pos * 3);
  if (pos < 170) { pos -= 85; return strip.Color(0, pos * 3, 255 - pos * 3); }
  pos -= 170;
  return strip.Color(pos * 3, 255 - pos * 3, 0);
}

void applyLedMode(String mode) {
  if (mode != "off" && mode != "static" && mode != "running" &&
      mode != "disco" && mode != "bounce") return;
  ledMode = mode;
  if (mode != "off") lastLedEffect = mode;

  if (mode == "off") {
    strip.clear();
    strip.show();
  } else if (mode == "static") {
    for (int i = 0; i < NUM_LEDS; i++) {
      int hue = (i * 256 / NUM_LEDS) & 255;
      strip.setPixelColor(i, wheelColor(hue));
    }
    strip.show();
  } else if (mode == "bounce") {
    bouncePos = 0;
    bounceDir = 1;
  }

  if (prefs.getString("ledMode", "") != ledMode) {
    prefs.putString("ledMode", ledMode);
  }
}

void handleLedAnimation() {
  if (ledMode == "running") {
    if (millis() - lastLedStep < 20) return;
    lastLedStep = millis();
    for (int i = 0; i < NUM_LEDS; i++) {
      int hue = ((i * 256 / NUM_LEDS) + rainbowStep) & 255;
      strip.setPixelColor(i, wheelColor(hue));
    }
    strip.show();
    rainbowStep += 3;
    if (rainbowStep >= 256) rainbowStep = 0;
  } else if (ledMode == "disco") {
    if (millis() - lastLedStep < 120) return;
    lastLedStep = millis();
    for (int i = 0; i < NUM_LEDS; i++) {
      strip.setPixelColor(i, strip.Color(random(0, 256), random(0, 256), random(0, 256)));
    }
    strip.show();
  } else if (ledMode == "bounce") {
    if (millis() - lastLedStep < 30) return;
    lastLedStep = millis();
    strip.clear();
    const int tailLen = 4;
    for (int t = 0; t < tailLen; t++) {
      int pos = bouncePos - (bounceDir * t);
      if (pos >= 0 && pos < NUM_LEDS) {
        int fade = 255 - (t * (255 / tailLen));
        uint32_t c = wheelColor((bouncePos * 8) & 255);
        uint8_t r = (uint8_t)(((c >> 16) & 0xFF) * fade / 255);
        uint8_t g = (uint8_t)(((c >> 8) & 0xFF) * fade / 255);
        uint8_t b = (uint8_t)((c & 0xFF) * fade / 255);
        strip.setPixelColor(pos, strip.Color(r, g, b));
      }
    }
    strip.show();
    bouncePos += bounceDir;
    if (bouncePos >= NUM_LEDS - 1 || bouncePos <= 0) bounceDir = -bounceDir;
  }
}

String buildStatusJson(bool includeSecret) {
  unsigned long runtime = millis() - startMillis;
  long s = runtime / 1000, m = s / 60, h = m / 60;
  String uptime = String(h) + ":" + String(m % 60) + ":" + String(s % 60);

  JsonDocument doc;
  doc["deviceId"] = deviceId;
  doc["setVoltage"] = currentSetVoltage;
  doc["ledMode"] = ledMode;
  doc["uptime"] = uptime;
  doc["chargerWatt"] = chargerwatt;
  doc["powerGood"] = pgood;
  doc["ch224aReady"] = ch224aReady;
  doc["fanSpeed"] = fanSpeedPercent;
  doc["fanRpm"] = fanRpm;
  doc["netMode"] = netMode;
  doc["configApActive"] = configApActive;
  doc["wifiConnected"] = wifiControlActive;
  if (wifiControlActive) {
    doc["ssid"] = WiFi.SSID();
    doc["ip"] = WiFi.localIP().toString();
  }

  if (includeSecret) doc["httpAuthPass"] = httpAuthPass;
  String jsonStr;
  serializeJson(doc, jsonStr);
  return jsonStr;
}

void publishStatusBLE() {
  if (deviceConnected && pCharacteristic != nullptr) {
    String jsonStr = buildStatusJson(true);
    pCharacteristic->setValue(jsonStr);
    pCharacteristic->notify();
  }
}

void processCommandJson(const String& cmd) {
  JsonDocument doc;
  if (deserializeJson(doc, cmd)) return;
  if (doc["voltage"].is<float>()) {
    applyVoltage(doc["voltage"]);
    triggerCmdBlink();
  }
  if (doc["ledMode"].is<const char*>()) {
    applyLedMode(doc["ledMode"].as<String>());
    triggerCmdBlink();
  }
  if (doc["fanSpeed"].is<int>()) {
    setFanSpeed(doc["fanSpeed"]);
    triggerCmdBlink();
  }
}

void handleScanWifi() {
  int n = WiFi.scanComplete();
  if (n == -2) {
    WiFi.scanNetworks(true);
    server.send(200, "text/plain", "scanning");
    return;
  }
  if (n == -1) {
    server.send(200, "text/plain", "scanning");
    return;
  }
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
  }
  String out;
  serializeJson(doc, out);
  WiFi.scanDelete();
  server.send(200, "application/json", out);
}

void handleSetWifi() {
  // Saat sedang mode AP config ("ESP32-Config"), tidak perlu HTTP auth lagi -
  // akses ke sini sudah otomatis terbatas cuma untuk HP yang tahu password
  // hotspot-nya. Ini yang memungkinkan setup WiFi langsung lewat browser
  // (buka alamat IP ESP32) tanpa perlu buka aplikasi/BLE sama sekali.
  // Kalau dipanggil saat ESP32 sudah tersambung ke WiFi rumah (bukan lagi di
  // mode AP config), tetap wajib HTTP auth supaya orang lain di jaringan yang
  // sama tidak bisa diam-diam mengganti WiFi ESP32.
  if (!configApActive && !checkHttpAuth()) return;
  if (!server.hasArg("ssid") || !server.hasArg("password")) {
    server.send(400, "text/plain", "missing ssid/password");
    return;
  }
  String ssid = server.arg("ssid");
  String pass = server.arg("password");
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.putString("netMode", "wifi");
  JsonDocument doc;
  doc["result"] = "OK";
  doc["deviceId"] = deviceId;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
  Serial.println("Kredensial WiFi disimpan (" + ssid + "), restart untuk masuk mode WiFi...");
  delay(400);
  ESP.restart();
}

static const char SETUP_PAGE_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="id">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#080b14">
<title>ESP32 Cooler • WiFi Setup</title>
<style>
:root{
  --bg:#080b14;--panel:#101522;--panel2:#0c111c;--line:#202a3b;
  --text:#f5f7fb;--muted:#8d98aa;--accent:#42e8c0;--accent2:#6b8cff;
  --danger:#ff6b78;--warn:#ffc857;--shadow:0 20px 60px rgba(0,0,0,.32);
}
*{box-sizing:border-box}
html{background:var(--bg);scroll-behavior:smooth}
body{
  margin:0;min-height:100vh;color:var(--text);
  font-family:Inter,-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Arial,sans-serif;
  background:
    radial-gradient(900px 420px at 50% -10%,rgba(67,232,192,.10),transparent 60%),
    radial-gradient(700px 500px at 100% 35%,rgba(107,140,255,.08),transparent 60%),
    var(--bg);
}
button,input{font:inherit}
button{border:0}
.container{width:min(100%,720px);margin:auto;padding:18px 16px 42px}
.topbar{display:flex;align-items:center;justify-content:space-between;margin:4px 2px 22px}
.brand{display:flex;align-items:center;gap:11px}
.brand-mark{
  width:42px;height:42px;border-radius:13px;display:grid;place-items:center;
  background:linear-gradient(145deg,#18352f,#14233a);border:1px solid #2b4650;
  box-shadow:0 8px 24px rgba(66,232,192,.08)
}
.brand-mark svg{width:22px;height:22px;stroke:var(--accent)}
.brand-title{font-size:15px;font-weight:800;letter-spacing:.2px}
.brand-sub{font-size:11px;color:var(--muted);margin-top:2px}
.status-pill{
  display:flex;align-items:center;gap:7px;padding:8px 11px;border-radius:999px;
  background:#0d1719;border:1px solid #1e4039;color:#9debd9;font-size:11px;font-weight:700
}
.status-dot{width:7px;height:7px;border-radius:50%;background:var(--accent);box-shadow:0 0 12px var(--accent)}
.hero{margin-bottom:16px}
.eyebrow{font-size:11px;text-transform:uppercase;letter-spacing:1.5px;color:var(--accent);font-weight:800;margin-bottom:8px}
h1{font-size:clamp(25px,7vw,34px);line-height:1.08;margin:0 0 9px;letter-spacing:-.8px}
.hero p{margin:0;color:var(--muted);font-size:13px;line-height:1.65;max-width:590px}
.card{
  background:linear-gradient(180deg,rgba(18,24,37,.98),rgba(13,18,29,.98));
  border:1px solid var(--line);border-radius:22px;padding:18px;margin-bottom:14px;
  box-shadow:var(--shadow);overflow:hidden
}
.card-head{display:flex;justify-content:space-between;align-items:center;gap:12px;margin-bottom:15px}
.card-title{font-size:14px;font-weight:800}
.card-note{font-size:11px;color:var(--muted);margin-top:3px}
.icon{
  width:36px;height:36px;border-radius:11px;background:#121d2c;border:1px solid #253247;
  display:grid;place-items:center;flex:0 0 auto
}
.icon svg{width:18px;height:18px;stroke:var(--accent)}
.info-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.info{
  min-width:0;background:var(--panel2);border:1px solid #1b2636;border-radius:15px;padding:13px
}
.label{font-size:10px;color:var(--muted);text-transform:uppercase;letter-spacing:1px;font-weight:700}
.value-row{display:flex;align-items:center;gap:8px;margin-top:7px}
.value{font-size:14px;font-weight:800;letter-spacing:.4px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.copy{
  margin-left:auto;width:28px;height:28px;border-radius:9px;background:#172132;color:#aeb9ca;
  border:1px solid #26344a;display:grid;place-items:center;cursor:pointer;flex:0 0 auto
}
.copy:active{transform:scale(.96)}
.copy svg{width:14px;height:14px}
.network{display:none;margin-top:10px;padding:11px 13px;border-radius:13px;background:#0c1918;border:1px solid #1d4039}
.network.show{display:flex;align-items:center;justify-content:space-between;gap:12px}
.network-label{font-size:10px;color:#7fd8c7;text-transform:uppercase;letter-spacing:1px;font-weight:800}
.network-value{font-size:12px;font-weight:700;margin-top:3px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.step{
  display:flex;gap:12px;align-items:flex-start;padding:12px 0;border-bottom:1px solid #1b2432
}
.step:last-child{border-bottom:0;padding-bottom:0}
.step:first-child{padding-top:0}
.step-num{
  width:27px;height:27px;border-radius:9px;background:#172233;border:1px solid #29384d;
  display:grid;place-items:center;color:#aebbd0;font-size:11px;font-weight:800;flex:0 0 auto
}
.step strong{font-size:12px;display:block;margin-bottom:3px}
.step span{font-size:11px;color:var(--muted);line-height:1.5}
.scan{
  width:100%;height:48px;border-radius:14px;background:#151e2d;border:1px solid #29364b;
  color:var(--text);font-size:13px;font-weight:800;cursor:pointer;display:flex;align-items:center;
  justify-content:center;gap:9px;transition:.18s
}
.scan:hover{border-color:#3d536d;background:#192436}
.scan:active{transform:translateY(1px)}
.scan svg{width:17px;height:17px;stroke:var(--accent)}
.scan.loading svg{animation:spin .8s linear infinite}
#wifiList{margin-top:11px}
.wifi-item{
  display:flex;align-items:center;gap:11px;width:100%;padding:12px;border-radius:14px;
  background:#0d131e;border:1px solid #1c2737;margin-bottom:7px;cursor:pointer;text-align:left;
  color:var(--text);transition:.18s
}
.wifi-item:hover{border-color:#36516a;background:#101927}
.wifi-icon{width:34px;height:34px;border-radius:10px;background:#13202b;display:grid;place-items:center;flex:0 0 auto}
.wifi-icon svg{width:17px;height:17px;stroke:#9aa9bb}
.wifi-main{min-width:0;flex:1}
.wifi-name{font-size:12px;font-weight:750;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.wifi-meta{font-size:10px;color:var(--muted);margin-top:3px}
.wifi-arrow{color:#647187;font-size:18px}
.field{margin-top:13px}
.field-label{display:block;font-size:11px;font-weight:750;color:#b7c1d0;margin:0 0 7px 2px}
.input-wrap{position:relative}
input{
  width:100%;height:49px;padding:0 44px 0 14px;border-radius:14px;border:1px solid #273448;
  background:#0a101a;color:var(--text);font-size:13px;outline:none;transition:.18s
}
input::placeholder{color:#5e6b7f}
input:focus{border-color:#3f8f82;box-shadow:0 0 0 3px rgba(66,232,192,.08)}
.toggle-pass{
  position:absolute;right:6px;top:6px;width:37px;height:37px;border-radius:10px;
  background:transparent;color:#748196;cursor:pointer;display:grid;place-items:center
}
.toggle-pass svg{width:17px;height:17px}
.primary{
  width:100%;height:51px;margin-top:14px;border-radius:15px;cursor:pointer;
  background:linear-gradient(100deg,#37dcb9,#5f86ff);color:#071019;font-size:13px;font-weight:900;
  box-shadow:0 12px 28px rgba(76,153,255,.18);transition:.18s
}
.primary:hover{filter:brightness(1.06)}
.primary:active{transform:translateY(1px)}
.primary:disabled{opacity:.55;cursor:not-allowed}
#status{min-height:0;margin-top:11px;font-size:11px;line-height:1.55}
.alert{padding:11px 12px;border-radius:12px}
.ok{background:#0d1e1b;border:1px solid #214c42;color:#91e8d6}
.err{background:#241317;border:1px solid #56303a;color:#ff9aa4}
.muted{color:var(--muted)}
.empty{padding:14px;text-align:center;color:var(--muted);font-size:11px;border:1px dashed #263246;border-radius:13px}
.footer{text-align:center;color:#566276;font-size:10px;padding:7px 0 0}
.hidden{display:none!important}
@keyframes spin{to{transform:rotate(360deg)}}
@media(max-width:520px){
  .container{padding:15px 12px 32px}
  .card{padding:15px;border-radius:19px}
  .info-grid{grid-template-columns:1fr}
  .status-pill{padding:7px 9px}
  .brand-mark{width:39px;height:39px}
}
</style>
</head>
<body>
<div class="container">

  <header class="topbar">
    <div class="brand">
      <div class="brand-mark">
        <svg viewBox="0 0 24 24" fill="none" stroke-width="1.8">
          <path d="M12 3v18M7 6.5l10 11M17 6.5l-10 11M4.5 12h15"/>
          <circle cx="12" cy="12" r="3.2"/>
        </svg>
      </div>
      <div>
        <div class="brand-title">ESP32 Cooler</div>
        <div class="brand-sub">Device configuration</div>
      </div>
    </div>
    <div class="status-pill"><span class="status-dot"></span>READY</div>
  </header>

  <section class="hero">
    <div class="eyebrow">Network setup</div>
    <h1>Hubungkan cooler ke WiFi</h1>
    <p>Pilih jaringan di sekitar, masukkan password, lalu simpan. Setelah tersambung, perangkat siap dikontrol dari jaringan lokal.</p>
  </section>

  <section class="card">
    <div class="card-head">
      <div>
        <div class="card-title">Informasi perangkat</div>
        <div class="card-note">Gunakan data ini saat menambahkan perangkat di aplikasi.</div>
      </div>
      <div class="icon">
        <svg viewBox="0 0 24 24" fill="none" stroke-width="1.8">
          <rect x="4" y="3" width="16" height="18" rx="3"/>
          <path d="M8 7h8M8 11h8M8 15h4"/>
        </svg>
      </div>
    </div>

    <div class="info-grid">
      <div class="info">
        <div class="label">Device ID</div>
        <div class="value-row">
          <div class="value" id="deviceId">—</div>
          <button class="copy" onclick="copyValue('deviceId')" aria-label="Salin Device ID">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8">
              <rect x="8" y="8" width="11" height="11" rx="2"/>
              <path d="M16 8V6a2 2 0 0 0-2-2H6a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h2"/>
            </svg>
          </button>
        </div>
      </div>

      <div class="info">
        <div class="label">Password kontrol</div>
        <div class="value-row">
          <div class="value" id="authPass">—</div>
          <button class="copy" onclick="copyValue('authPass')" aria-label="Salin password kontrol">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8">
              <rect x="8" y="8" width="11" height="11" rx="2"/>
              <path d="M16 8V6a2 2 0 0 0-2-2H6a2 2 0 0 0 2 2v8"/>
            </svg>
          </button>
        </div>
      </div>
    </div>

    <div class="network" id="rowNet">
      <div>
        <div class="network-label">Jaringan aktif</div>
        <div class="network-value" id="netInfo">—</div>
      </div>
      <span style="color:#42e8c0;font-size:16px">●</span>
    </div>
  </section>

  <section class="card">
    <div class="card-head">
      <div>
        <div class="card-title">Cara cepat</div>
        <div class="card-note">Selesaikan tiga langkah berikut.</div>
      </div>
      <div class="icon">
        <svg viewBox="0 0 24 24" fill="none" stroke-width="1.8">
          <path d="M12 3l2.3 5.1L20 10.3l-5.7 2.2L12 18l-2.3-5.5L4 10.3l5.7-2.2L12 3z"/>
          <path d="M19 17v4M17 19h4"/>
        </svg>
      </div>
    </div>
    <div class="step">
      <div class="step-num">01</div>
      <div><strong>Pilih jaringan</strong><span>Tekan tombol scan untuk melihat WiFi yang tersedia di sekitar.</span></div>
    </div>
    <div class="step">
      <div class="step-num">02</div>
      <div><strong>Masukkan password</strong><span>Tap nama WiFi, lalu isi password jaringan tersebut.</span></div>
    </div>
    <div class="step">
      <div class="step-num">03</div>
      <div><strong>Hubungkan</strong><span>ESP32 akan menyimpan konfigurasi dan melakukan restart otomatis.</span></div>
    </div>
  </section>

  <section class="card">
    <div class="card-head">
      <div>
        <div class="card-title">Pilih jaringan WiFi</div>
        <div class="card-note">Jaringan terkuat akan ditampilkan lebih dahulu.</div>
      </div>
      <div class="icon">
        <svg viewBox="0 0 24 24" fill="none" stroke-width="1.8">
          <path d="M3 9.5a14 14 0 0 1 18 0M6.5 13a8.7 8.7 0 0 1 11 0M10 16.5a3.7 3.7 0 0 1 4 0"/>
          <circle cx="12" cy="20" r="1"/>
        </svg>
      </div>
    </div>

    <button class="scan" id="scanBtn" onclick="scanWifi()">
      <svg viewBox="0 0 24 24" fill="none" stroke-width="2">
        <circle cx="11" cy="11" r="6.5"/><path d="M16 16l5 5"/>
      </svg>
      <span id="scanText">Cari WiFi sekitar</span>
    </button>

    <div id="wifiList"></div>

    <div class="field">
      <label class="field-label" for="ssid">Nama WiFi</label>
      <input type="text" id="ssid" autocomplete="off" autocapitalize="none" placeholder="Pilih jaringan atau ketik SSID">
    </div>

    <div class="field">
      <label class="field-label" for="password">Password WiFi</label>
      <div class="input-wrap">
        <input type="password" id="password" autocomplete="off" placeholder="Masukkan password WiFi">
        <button class="toggle-pass" type="button" onclick="togglePassword()" aria-label="Tampilkan password">
          <svg id="eyeIcon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8">
            <path d="M2.5 12s3.5-6 9.5-6 9.5 6 9.5 6-3.5 6-9.5 6-9.5-6-9.5-6z"/>
            <circle cx="12" cy="12" r="2.5"/>
          </svg>
        </button>
      </div>
    </div>

    <button class="primary" id="connectBtn" onclick="connectWifi()">Hubungkan ke WiFi</button>
    <div id="status"></div>
  </section>

  <div class="footer">ESP32 Cooler • Local configuration</div>
</div>

<script>
const $ = id => document.getElementById(id);

function setStatus(message, type){
  const el = $('status');
  if(!message){el.innerHTML='';el.className='';return;}
  el.innerHTML = '<div class="alert '+(type==='ok'?'ok':'err')+'">'+message+'</div>';
}

function escapeHtml(s){
  return String(s).replace(/[&<>"']/g,m=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#039;'}[m]));
}

async function loadDeviceInfo(){
  try{
    const r = await fetch('/status',{cache:'no-store'});
    const j = await r.json();
    $('deviceId').textContent = j.deviceId || '—';
    $('authPass').textContent = j.httpAuthPass || '—';
    if(j.wifiConnected){
      $('rowNet').classList.add('show');
      $('netInfo').textContent = (j.ssid || 'WiFi')+' • '+(j.ip || '—');
    }
  }catch(e){
    $('deviceId').textContent='—';
    $('authPass').textContent='—';
  }
}

async function copyValue(id){
  const text=$(id).textContent.trim();
  if(!text || text==='—') return;
  try{
    await navigator.clipboard.writeText(text);
    const old=$('#'+id).parentElement.querySelector('.copy');
    old.style.borderColor='#42e8c0';
    setTimeout(()=>old.style.borderColor='',900);
  }catch(e){}
}

function togglePassword(){
  const input=$('password');
  input.type=input.type==='password'?'text':'password';
}

function signalLabel(rssi){
  if(rssi >= -55) return 'Sinyal sangat kuat';
  if(rssi >= -67) return 'Sinyal kuat';
  if(rssi >= -75) return 'Sinyal cukup';
  return 'Sinyal lemah';
}

async function scanWifi(){
  const list=$('wifiList'),btn=$('scanBtn'),text=$('scanText');
  btn.classList.add('loading');btn.disabled=true;text.textContent='Memindai jaringan…';
  list.innerHTML='<div class="empty">Mencari jaringan WiFi di sekitar…</div>';
  try{
    let networks=null;
    for(let i=0;i<14;i++){
      const r=await fetch('/scanwifi',{cache:'no-store'});
      const ct=r.headers.get('content-type')||'';
      if(ct.includes('application/json')){networks=await r.json();break;}
      await new Promise(res=>setTimeout(res,800));
    }
    if(!networks){list.innerHTML='<div class="empty">Pemindaian gagal. Coba lagi.</div>';return;}
    networks.sort((a,b)=>(b.rssi||-100)-(a.rssi||-100));
    if(!networks.length){list.innerHTML='<div class="empty">Tidak ada jaringan WiFi yang ditemukan.</div>';return;}
    list.innerHTML='';
    const seen=new Set();
    networks.forEach(n=>{
      if(!n.ssid || seen.has(n.ssid)) return;
      seen.add(n.ssid);
      const div=document.createElement('button');
      div.className='wifi-item';
      div.type='button';
      div.innerHTML=
        '<div class="wifi-icon"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8">'+
        '<path d="M3 9.5a14 14 0 0 1 18 0M6.5 13a8.7 8.7 0 0 1 11 0M10 16.5a3.7 3.7 0 0 1 4 0"/><circle cx="12" cy="20" r="1"/></svg></div>'+
        '<div class="wifi-main"><div class="wifi-name">'+escapeHtml(n.ssid)+'</div>'+
        '<div class="wifi-meta">'+(n.rssi??'—')+' dBm • '+signalLabel(n.rssi||-100)+'</div></div>'+
        '<div class="wifi-arrow">›</div>';
      div.onclick=()=>{
        $('ssid').value=n.ssid;
        $('password').focus();
        setStatus('', '');
      };
      list.appendChild(div);
    });
  }catch(e){
    list.innerHTML='<div class="empty">Tidak dapat memindai jaringan. Pastikan HP masih terhubung ke ESP32.</div>';
  }finally{
    btn.classList.remove('loading');btn.disabled=false;text.textContent='Cari WiFi sekitar';
  }
}

async function connectWifi(){
  const ssid=$('ssid').value.trim(),password=$('password').value;
  if(!ssid){setStatus('Pilih atau masukkan nama WiFi terlebih dahulu.','err');$('ssid').focus();return;}
  const btn=$('connectBtn');
  btn.disabled=true;btn.textContent='Menyimpan konfigurasi…';setStatus('', '');
  try{
    const body='ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(password);
    const r=await fetch('/setwifi',{
      method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body
    });
    if(!r.ok) throw new Error('request failed');
    setStatus('<strong>Konfigurasi tersimpan.</strong><br>ESP32 sedang restart dan mencoba terhubung ke WiFi. Setelah itu, sambungkan HP ke WiFi yang sama untuk melanjutkan dari aplikasi.','ok');
  }catch(e){
    setStatus('<strong>Perintah sudah dikirim.</strong><br>Koneksi ke halaman akan terputus saat ESP32 restart. Ini normal. Sambungkan HP kembali ke WiFi rumah setelah beberapa saat.','ok');
  }finally{
    setTimeout(()=>{btn.disabled=false;btn.textContent='Hubungkan ke WiFi';},2500);
  }
}

loadDeviceInfo();
</script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send_P(200, "text/html", SETUP_PAGE_HTML);
}

void handleStatusHttp() {
  lastAppContact = millis();
  // includeSecret hanya true kalau masih di mode AP config — akses ke sini
  // sudah otomatis terbatas cuma untuk yang tahu password hotspot
  // ESP32-Config, jadi aman ditampilkan di halaman setup browser. Begitu
  // sudah pindah ke WiFi rumah, secret ini TIDAK PERNAH dikirim lewat HTTP
  // lagi (harus lewat BLE yang terenkripsi).
  server.send(200, "application/json", buildStatusJson(configApActive));
}

void handleSetCmd() {
  if (!checkHttpAuth()) return;
  lastAppContact = millis();

  JsonDocument doc;
  if (server.hasArg("voltage")) doc["voltage"] = server.arg("voltage").toFloat();
  if (server.hasArg("ledMode")) doc["ledMode"] = server.arg("ledMode");
  if (server.hasArg("fanSpeed")) doc["fanSpeed"] = server.arg("fanSpeed").toInt();
  if (server.hasArg("action")) doc["action"] = server.arg("action");

  String cmd;
  serializeJson(doc, cmd);

  // Jalankan perintah TERLEBIH DAHULU, lalu kirim snapshot status aktual.
  // Dengan cara ini aplikasi WiFi tidak perlu menunggu polling /status
  // untuk mengetahui bahwa voltage sudah berubah.
  processCommandJson(cmd);

  String statusJson = buildStatusJson(false);
  server.send(200, "application/json", statusJson);
}

void handleSwitchBle() {
  if (!checkHttpAuth()) return;
  server.send(200, "text/plain", "OK");
  prefs.putString("netMode", "ble");
  Serial.println("Pindah ke mode Bluetooth, restart...");
  delay(400);
  ESP.restart();
}

void registerHttpHandlers() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/scanwifi", HTTP_GET, handleScanWifi);
  server.on("/setwifi", HTTP_POST, handleSetWifi);
  server.on("/status", HTTP_GET, handleStatusHttp);
  server.on("/set", HTTP_POST, handleSetCmd);
  server.on("/switch_ble", HTTP_POST, handleSwitchBle);
}

void startConfigAP() {
  if (configApActive) return;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  server.begin();
  configApActive = true;
  Serial.println("Config AP aktif: " + String(AP_SSID) + " @ " + WiFi.softAPIP().toString());
}

void sendUdpBeacon() {
  JsonDocument doc;
  doc["deviceId"] = deviceId;
  doc["ip"] = WiFi.localIP().toString();
  String out;
  serializeJson(doc, out);
  udp.beginPacket(IPAddress(255, 255, 255, 255), UDP_BEACON_PORT);
  udp.write((const uint8_t*)out.c_str(), out.length());
  udp.endPacket();
}

void startWifiControlMode(const String& ssid, const String& pass) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.println("Menghubungkan ke WiFi: " + ssid);
  unsigned long attemptStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - attemptStart < 15000) {
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nSUKSES: WiFi tersambung, IP: " + WiFi.localIP().toString());
    WiFi.setSleep(false); // matikan modem-sleep — sering jadi penyebab WiFi ESP32 putus sendiri
    server.begin();
    udp.begin(UDP_BEACON_PORT);
    wifiControlActive = true;
  } else {
    Serial.println("\nGAGAL: Tidak bisa konek WiFi dalam 15 detik, kembali ke mode Bluetooth...");
    prefs.putString("netMode", "ble");
    prefs.putString("ssid", "");
    prefs.putString("pass", "");
    delay(300);
    ESP.restart();
  }
}

void startBleMode() {
  Serial.println("Menginisialisasi Bluetooth (NimBLE)...");

  NimBLEDevice::init(bleName.c_str());

  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  #ifdef ESP_PWR_LVL_P9
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  #else
    NimBLEDevice::setPower(9);
  #endif

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  NimBLEService *pService = pServer->createService(SERVICE_UUID);

  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC |
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC |
    NIMBLE_PROPERTY::NOTIFY
  );
  pCharacteristic->setCallbacks(new MyCharacteristicCallbacks());
  pService->start();

  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setAppearance(0x0000);

  NimBLEAdvertisementData scanResponseData;
  scanResponseData.setName(bleName.c_str());
  pAdvertising->setScanResponseData(scanResponseData);
  pAdvertising->enableScanResponse(true);

  bool advSuccess = pAdvertising->start();
  if (advSuccess) {
    Serial.println("SUKSES: BLE Advertising aktif dengan nama: " + bleName);
  } else {
    Serial.println("GAGAL: BLE Advertising gagal dimulai!");
  }
}

void handleBleAction(const String& action) {
  if (action == "start_wifi_setup") {
    NimBLEDevice::deinit(true);
    deviceConnected = false;
    pCharacteristic = nullptr;
    pServer = nullptr;
    delay(300);
    startConfigAP();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, INPUT_PULLUP);
  Wire.begin(SDA_PIN, SCL_PIN);
  scanI2CBus();
  deviceId = computeDeviceId();
  bleName = "ESP32-Cooler-" + deviceId;

  prefs.begin("cooler", false);
  netMode = prefs.getString("netMode", "ble");
  savedSsid = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");
  httpAuthPass = loadOrCreateHttpAuthPass();

  // Tombol darurat: tahan tombol BOOT fisik di board sambil menyalakan/reset
  // ESP32 untuk memaksa balik ke mode Bluetooth + AP config, walaupun
  // sebelumnya tersimpan di mode WiFi. Ini jalan keluar kalau device
  // kejebak di mode WiFi (mis. WiFi berhasil connect tapi app gagal sync)
  // dan BLE/AP config jadi tidak bisa diakses sama sekali - tanpa ini,
  // satu-satunya jalan keluar adalah erase flash total.
  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);
  if (digitalRead(BOOT_BTN_PIN) == LOW) {
    Serial.println("Tombol BOOT ditahan saat menyala - paksa balik ke mode Bluetooth + AP config.");
    netMode = "ble";
    prefs.putString("netMode", "ble");
  }

  float savedVoltage = prefs.getFloat("voltage", 5.0);
  int savedFanSpeed = prefs.getInt("fanSpeed", 100);
  String savedLedMode = prefs.getString("ledMode", "off");

  pinMode(PIN_ONBOARD_LED, OUTPUT);
  onboardLedWrite(false);

  ledcAttach(FAN_PWM_PIN, FAN_PWM_FREQ_HZ, FAN_PWM_RESOLUTION);
  setFanSpeed(savedFanSpeed);

  pinMode(FAN_TACH_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FAN_TACH_PIN), fanTachISR, FALLING);
  lastFanRpmCalc = millis();

  ch224aReady = ch224Begin();
  if (!ch224aReady) {
    Serial.println("CH224 not responding! Melanjutkan tanpa kontrol PD, akan dicoba lagi di background...");
  } else {
    Serial.print("CH224 terdeteksi di alamat I2C 0x");
    Serial.println(ch224Addr, HEX);
    applyVoltage(savedVoltage);
  }

  strip.begin();
  strip.setBrightness(80);
  strip.show();
  applyLedMode(savedLedMode);

  registerHttpHandlers();

  if (netMode == "wifi" && savedSsid.length() > 0) {
    startWifiControlMode(savedSsid, savedPass);
  } else {
    // AP config ("ESP32-Config") selalu dinyalakan di sini, terlepas dari
    // ada/tidaknya SSID rumah yang tersimpan sebelumnya. Ini supaya halaman
    // setup WiFi lewat browser (http://192.168.4.1) selalu bisa diakses
    // langsung tanpa perlu buka aplikasi/BLE dulu - cukup tekan tombol BOOT
    // saat menyalakan ESP32 untuk masuk ke mode ini kapan saja.
    startBleMode();
    startConfigAP();
  }

  startMillis = millis();
}

void loop() {
  char cmdBuf[129] = {0};
  portENTER_CRITICAL(&bleMux);

  if (bleWritePending) {
    memcpy(cmdBuf, bleCommandBuf, bleCommandLen);
    cmdBuf[bleCommandLen] = '\0';
    bleCommandLen = 0;
    bleCommandBuf[0] = '\0';
    bleWritePending = false;
  }
  portEXIT_CRITICAL(&bleMux);
  String cmd = String(cmdBuf);

  if (cmd.length() > 0) {
    processCommandJson(cmd);

    JsonDocument doc;
    if (!deserializeJson(doc, cmd) && doc["action"].is<const char*>()) {
      handleBleAction(doc["action"].as<String>());
    }
  }

  if (configApActive || wifiControlActive) {
    server.handleClient();
  }

  if (wifiControlActive && millis() - lastBeacon > 2000) {
    sendUdpBeacon();
    lastBeacon = millis();
  }

  static unsigned long lastWifiCheck = 0;
  static unsigned long wifiDownSince = 0;
  if (wifiControlActive && millis() - lastWifiCheck > 2000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      if (wifiDownSince == 0) {
        wifiDownSince = millis();
        Serial.println("WiFi terputus, mencoba reconnect...");
        WiFi.reconnect();
      } else if (millis() - wifiDownSince > 20000) {        
        Serial.println("WiFi tidak pulih dalam 20 detik, kembali ke mode Bluetooth...");
        prefs.putString("netMode", "ble");
        delay(300);
        ESP.restart();
      }
    } else {
      wifiDownSince = 0;
    }
  }

  handleStatusLed();
  handleLedAnimation();

  if (millis() - lastFanRpmCalc >= 1000) {
    updateFanRpm();
  }

  static unsigned long lastCh224Read = 0;
  static unsigned long lastCh224Retry = 0;
  if (ch224aReady) {
    if (millis() - lastCh224Read > 200) {
      lastCh224Read = millis();
      pgood = CH224X1->isPowerGood();
      current = CH224X1->getCurrentProfile() / 1000.0;
      chargerwatt = current * currentSetVoltage;
      Serial.print("Maximum current : ");
      Serial.print(current, 0);
      Serial.println(" A)");
      Serial.print("Available power : ");
      Serial.print(chargerwatt);
      Serial.println(" W");
      Serial.print("power good : ");
      Serial.println(pgood);
    }
  } else if (millis() - lastCh224Retry > 3000) {
    lastCh224Retry = millis();
    Serial.println("Mencoba deteksi ulang CH224A...");
    ch224aReady = ch224Begin();
    if (ch224aReady) {
      Serial.println("CH224A terdeteksi.");
      CH224X1->setVoltage(0);
    }
  }

  if (millis() - lastPublish > 300) {
    publishStatusBLE();
    lastPublish = millis();
  }
}
