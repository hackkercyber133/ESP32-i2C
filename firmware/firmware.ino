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
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Setup WiFi ESP32 Cooler</title>
<style>
  :root{--bg:#05050f;--card:#10122a;--accent:#3ee6c4;--accent2:#5b8cff;--text:#eef0ff;--faint:#8b8fb8;--danger:#ff6b6b;}
  *{box-sizing:border-box;}
  body{margin:0;background:linear-gradient(160deg,#05050f,#0a0d24 60%,#05050f);color:var(--text);
       font-family:-apple-system,Segoe UI,Roboto,Arial,sans-serif;padding:20px;min-height:100vh;}
  .card{background:var(--card);border:1px solid #23264d;border-radius:16px;padding:20px;margin-bottom:16px;
        box-shadow:0 8px 24px rgba(0,0,0,.35);}
  h1{font-size:19px;margin:0 0 4px;background:linear-gradient(90deg,var(--accent),var(--accent2));
     -webkit-background-clip:text;background-clip:text;color:transparent;}
  p.sub{color:var(--faint);font-size:12px;margin:0 0 18px;}
  .row{display:flex;align-items:center;justify-content:space-between;padding:6px 0;font-size:13px;}
  .row .k{color:var(--faint);}
  .row .v{font-weight:700;letter-spacing:.5px;}
  button{width:100%;padding:14px;border-radius:12px;border:none;font-size:14px;font-weight:800;
         letter-spacing:.5px;cursor:pointer;margin-top:6px;}
  .btn-primary{background:linear-gradient(90deg,var(--accent),var(--accent2));color:#04101a;}
  .btn-outline{background:transparent;border:1px solid #33375f;color:var(--text);}
  input{width:100%;padding:13px;border-radius:12px;border:1px solid #2a2e58;background:#0b0d24;
        color:var(--text);font-size:14px;margin-top:10px;}
  input:focus{outline:none;border-color:var(--accent);}
  #wifiList{margin-top:12px;}
  .wifi-item{display:flex;justify-content:space-between;align-items:center;padding:12px 14px;
             background:#0b0d24;border:1px solid #23264d;border-radius:10px;margin-bottom:8px;cursor:pointer;}
  .wifi-item:active{border-color:var(--accent);}
  .wifi-item .ssid{font-size:13px;}
  .wifi-item .rssi{font-size:11px;color:var(--faint);}
  #status{margin-top:14px;font-size:13px;line-height:1.5;min-height:18px;}
  .ok{color:var(--accent);}
  .err{color:var(--danger);}
  .spin{display:inline-block;width:14px;height:14px;border:2px solid #33375f;border-top-color:var(--accent);
        border-radius:50%;animation:sp .7s linear infinite;vertical-align:middle;margin-right:6px;}
  @keyframes sp{to{transform:rotate(360deg);}}
</style>
</head>
<body>

  <div class="card">
    <h1>⚙️ Setup WiFi ESP32 Cooler</h1>
    <p class="sub">Dibuka langsung dari browser, tidak perlu aplikasi.</p>
    <div class="row"><span class="k">Device ID</span><span class="v" id="deviceId">-</span></div>
    <div class="row"><span class="k">Password Kontrol</span><span class="v" id="authPass">-</span></div>
    <p class="sub" style="margin:6px 0 0;">⬆️ Catat 2 nilai di atas. Kalau nanti nambah cooler ini lewat menu "Manual (WiFi)" di aplikasi, kamu perlu masukkan keduanya supaya bisa mengontrol (bukan cuma lihat status).</p>
    <div class="row" id="rowNet" style="display:none;">
      <span class="k">Status Jaringan</span><span class="v" id="netInfo">-</span>
    </div>
  </div>

  <div class="card">
    <button class="btn-outline" onclick="scanWifi()">🔍 Cari WiFi Sekitar</button>
    <div id="wifiList"></div>

    <input type="text" id="ssid" placeholder="Nama WiFi (SSID)">
    <input type="password" id="password" placeholder="Password WiFi">
    <button class="btn-primary" onclick="connectWifi()">🔗 Hubungkan</button>
    <div id="status"></div>
  </div>

<script>
function setStatus(html, cls){
  const el = document.getElementById('status');
  el.innerHTML = html;
  el.className = cls || '';
}

async function loadDeviceInfo(){
  try{
    const r = await fetch('/status');
    const j = await r.json();
    document.getElementById('deviceId').textContent = j.deviceId || '-';
    document.getElementById('authPass').textContent = j.httpAuthPass || '-';
    if (j.wifiConnected){
      document.getElementById('rowNet').style.display = 'flex';
      document.getElementById('netInfo').textContent = (j.ssid || '-') + ' - ' + (j.ip || '-');
    }
  }catch(e){}
}

async function scanWifi(){
  const list = document.getElementById('wifiList');
  list.innerHTML = '<p style="color:#8b8fb8;font-size:12px;"><span class="spin"></span>Memindai jaringan...</p>';
  try{
    let networks = null;
    for (let i = 0; i < 12; i++){
      const r = await fetch('/scanwifi');
      const ct = r.headers.get('content-type') || '';
      if (ct.indexOf('application/json') !== -1){
        networks = await r.json();
        break;
      }
      await new Promise(res => setTimeout(res, 900));
    }
    if (!networks){
      list.innerHTML = '<p style="color:#ff6b6b;font-size:12px;">Gagal memindai, coba lagi.</p>';
      return;
    }
    if (networks.length === 0){
      list.innerHTML = '<p style="color:#8b8fb8;font-size:12px;">Tidak ada WiFi ditemukan.</p>';
      return;
    }
    networks.sort((a,b) => b.rssi - a.rssi);
    list.innerHTML = '';
    networks.forEach(n => {
      const div = document.createElement('div');
      div.className = 'wifi-item';
      div.innerHTML = '<span class="ssid">📶 ' + n.ssid + '</span><span class="rssi">' + n.rssi + ' dBm</span>';
      div.onclick = () => { document.getElementById('ssid').value = n.ssid; document.getElementById('password').focus(); };
      list.appendChild(div);
    });
  }catch(e){
    list.innerHTML = '<p style="color:#ff6b6b;font-size:12px;">Gagal memindai, coba lagi.</p>';
  }
}

async function connectWifi(){
  const ssid = document.getElementById('ssid').value.trim();
  const password = document.getElementById('password').value;
  if (!ssid){ setStatus('Isi nama WiFi dulu.', 'err'); return; }
  setStatus('<span class="spin"></span>Menyimpan & menghubungkan...', '');
  try{
    const body = 'ssid=' + encodeURIComponent(ssid) + '&password=' + encodeURIComponent(password);
    await fetch('/setwifi', {
      method: 'POST',
      headers: {'Content-Type':'application/x-www-form-urlencoded'},
      body: body
    });
    setStatus('✅ Tersimpan! ESP32 sedang restart & mencoba konek ke WiFi rumah (kurang lebih 15 detik).<br>' +
               'Sambungkan HP kembali ke WiFi rumah, lalu buka aplikasi, Tambah Cooler, tab Manual (WiFi), masukkan Device ID di atas.', 'ok');
  }catch(e){
    setStatus('✅ Perintah terkirim. ESP32 kemungkinan sudah restart untuk konek WiFi (koneksi ke halaman ini terputus, itu normal).<br>' +
               'Sambungkan HP kembali ke WiFi rumah lalu buka aplikasi.', 'ok');
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
  processCommandJson(cmd);
  server.send(200, "text/plain", "OK");
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
