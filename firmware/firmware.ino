#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <Adafruit_NeoPixel.h>
#include <ArduinoJson.h>

#include <Wire.h>
#include <CH224X_I2C.h>

String deviceId;
String bleName;

String computeDeviceId() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[7];
  snprintf(buf, sizeof(buf), "%06X", (unsigned int)(mac & 0xFFFFFF));
  return String(buf);
}

#define SDA_PIN      5
#define SCL_PIN      6
#define PG_PIN       7

CH224X_I2C CH224X1(Wire, 0x23, PG_PIN); // we use A7 as isPowerGood pin

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
#define ONBOARD_LED_ACTIVE_LOW true
bool onboardBlinkActive = false;
unsigned long onboardBlinkStart = 0;
const unsigned long ONBOARD_BLINK_MS = 150;

void onboardLedWrite(bool on) {
  digitalWrite(PIN_ONBOARD_LED, (ONBOARD_LED_ACTIVE_LOW ? !on : on) ? LOW : HIGH);
}

void triggerOnboardBlink() {
  onboardBlinkActive = true;
  onboardBlinkStart = millis();
  onboardLedWrite(true);
}

void handleOnboardBlink() {
  if (onboardBlinkActive && millis() - onboardBlinkStart >= ONBOARD_BLINK_MS) {
    onboardLedWrite(false);
    onboardBlinkActive = false;
  }
}

float currentSetVoltage = 5.0;
unsigned long startMillis = 0;
unsigned long lastPublish = 0;
float current = 0;      // arus maksimum charger dalam mA (dibaca dari CH224X1)
float chargerwatt = 0;  // watt maksimum = currentSetVoltage * (current/1000)
bool ch224aReady = false;
bool powerGood = false;
String pdStatus = "CH224A_NOT_READY";

// ===== THROTTLE POLLING I2C KE CH224X =====
// Sebelumnya firmware membaca I2C (getCurrentProfile + hasProtocol) di SETIAP
// iterasi loop() DAN di dalam critical section bersama BLE - ini menahan bus
// I2C dan mem-block interrupt tiap ~5ms, yang bikin BLE stack & respons app
// jadi lambat/patah-patah. Sekarang I2C dipoll berkala di luar critical section.
#define CH224_POLL_INTERVAL_MS 150
unsigned long lastCh224Poll = 0;
// Supaya auto-request 20V (saat PD baru terdeteksi) cuma jalan SEKALI, bukan
// tiap loop menimpa balik pilihan voltase manual dari app ke 20V terus-menerus.
bool pdAutoRequested = false;

Preferences prefs;
String netMode;
String savedSsid;
String savedPass;
bool wifiControlActive = false;
bool configApActive = false;

#define AP_SSID "ESP32-Config"
#define AP_PASS "12345678"

WebServer server(80);
WiFiUDP udp;
#define UDP_BEACON_PORT 47269
unsigned long lastBeacon = 0;

#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

NimBLEServer* pServer = nullptr;
NimBLECharacteristic* pCharacteristic = nullptr;
volatile bool bleWritePending = false;
portMUX_TYPE bleMux = portMUX_INITIALIZER_UNLOCKED;
bool deviceConnected = false;
char bleCommandBuf[129] = {0};
volatile uint16_t bleCommandLen = 0;

class MyServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* srv, NimBLEConnInfo& connInfo) override {
    deviceConnected = true;
    Serial.println("BLE: Terhubung ke App!");
    // Minta interval koneksi BLE lebih rapat (7.5-15ms) begitu app connect,
    // supaya notify status & write command lebih cepat sampai (default Android
    // sering minta interval lebih longgar ~30-50ms). Param dalam unit 1.25ms.
    srv->updateConnParams(connInfo.getConnHandle(), 6, 12, 0, 400);
  }

  void onDisconnect(NimBLEServer* srv, NimBLEConnInfo& connInfo, int reason) override {
    deviceConnected = false;
    Serial.println("BLE: Terputus, me-restart advertising...");
    NimBLEDevice::getAdvertising()->start();
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

bool applyVoltage(float volt) {
  if (volt >= 13.5) volt = 15.0;
  else if (volt >= 10.5) volt = 12.0;
  else if (volt >= 7.0) volt = 9.0;
  else volt = 5.0;

  uint8_t code;
  if (volt == 9.0) code = 1;
  else if (volt == 12.0) code = 2;
  else if (volt == 15.0) code = 3;
  else code = 0;

  CH224X1.setVoltage(code);
  currentSetVoltage = volt;
  // Pilihan manual dari app menang - jangan biarkan auto-negotiate PD di
  // pollCH224X() menimpanya balik ke 20V.
  pdAutoRequested = true;
  return true;
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

// Baca status decoy CH224X (arus maksimum charger, power-good, protokol).
// PENTING: dipanggil dengan throttle (lihat CH224_POLL_INTERVAL_MS) dan SELALU
// di luar critical section - transaksi I2C bisa kena clock-stretch/lambat, dan
// menjalankannya di dalam critical section (seperti kode lama) akan mem-block
// interrupt sehingga BLE/WiFi ikut macet.
void pollCH224X() {
  current = CH224X1.getCurrentProfile(); // mA
  chargerwatt = (current / 1000.0) * currentSetVoltage;
  powerGood = CH224X1.isPowerGood();
  pdStatus = powerGood ? "PD_NEGOTIATED" : "PD_WAITING";

  // Auto-request 20V HANYA sekali, saat PD pertama kali terdeteksi dan user
  // belum pernah memilih voltase manual sama sekali (pdAutoRequested masih
  // false sejak boot). Kode lama memanggil setVoltage(4) di SETIAP loop()
  // selama PD terdeteksi - itu terus-menerus menimpa balik pilihan voltase
  // manual dari app (5/9/12/15V) ke 20V setiap ~5ms, sehingga permintaan
  // voltase dari app terasa tidak direspons / balik sendiri. Sekarang cukup
  // sekali di awal saja, dan begitu applyVoltage() pernah dipanggil (dari app),
  // auto-request ini tidak akan menimpa lagi.
  if (!pdAutoRequested && CH224X1.hasProtocol(CH224X_I2C::PROTOCOL_PD)) {
    CH224X1.setVoltage(4); // request 20V profile
    currentSetVoltage = 20.0;
    pdAutoRequested = true;
    Serial.println("PD terdeteksi, request awal 20V...");
  }
}

String buildStatusJson() {
  unsigned long runtime = millis() - startMillis;
  long s = runtime / 1000, m = s / 60, h = m / 60;
  String uptime = String(h) + ":" + String(m % 60) + ":" + String(s % 60);

  JsonDocument doc;
  doc["deviceId"] = deviceId;
  doc["setVoltage"] = currentSetVoltage;
  doc["requestedVoltage"] = currentSetVoltage;
  doc["ledMode"] = ledMode;
  doc["uptime"] = uptime;
  doc["chargerWatt"] = chargerwatt;
  doc["powerGood"] = powerGood;
  doc["ch224aReady"] = ch224aReady;
  doc["pdStatus"] = pdStatus;
  String jsonStr;
  serializeJson(doc, jsonStr);
  return jsonStr;
}

void publishStatusBLE() {
  if (deviceConnected && pCharacteristic != nullptr) {
    String jsonStr = buildStatusJson();
    pCharacteristic->setValue(jsonStr);
    pCharacteristic->notify();
  }
}

void processCommandJson(const String& cmd) {
  JsonDocument doc;
  if (deserializeJson(doc, cmd)) return;
  if (doc["voltage"].is<float>()) {
    applyVoltage(doc["voltage"]);
    triggerOnboardBlink();
  }
  if (doc["ledMode"].is<const char*>()) applyLedMode(doc["ledMode"].as<String>());
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

void handleStatusHttp() {
  server.send(200, "application/json", buildStatusJson());
}

void handleSetCmd() {
  JsonDocument doc;
  if (server.hasArg("voltage")) doc["voltage"] = server.arg("voltage").toFloat();
  if (server.hasArg("ledMode")) doc["ledMode"] = server.arg("ledMode");
  if (server.hasArg("action")) doc["action"] = server.arg("action");
  String cmd;
  serializeJson(doc, cmd);
  processCommandJson(cmd);
  // Langsung balikin status TERBARU (bukan cuma "OK") supaya app WiFi tidak
  // perlu menunggu polling /status berikutnya (sebelumnya jeda sampai 3 detik)
  // untuk tahu hasil perintahnya.
  server.send(200, "application/json", buildStatusJson());
}

void handleSwitchBle() {
  server.send(200, "text/plain", "OK");
  prefs.putString("netMode", "ble");
  Serial.println("Pindah ke mode Bluetooth, restart...");
  delay(400);
  ESP.restart();
}

void registerHttpHandlers() {
  server.on("/scanwifi", handleScanWifi);
  server.on("/setwifi", handleSetWifi);
  server.on("/status", handleStatusHttp);
  server.on("/set", handleSetCmd);
  server.on("/switch_ble", handleSwitchBle);
}

void startConfigAP() {
  if (configApActive) return;

  WiFi.mode(wifiControlActive ? WIFI_AP_STA : WIFI_AP);
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
    server.begin();
    udp.begin(UDP_BEACON_PORT);
    wifiControlActive = true;
  } else {
    Serial.println("\nGAGAL: Tidak bisa konek WiFi dalam 15 detik, kembali ke mode Bluetooth...");
    prefs.putString("netMode", "ble");
    delay(300);
    ESP.restart();
  }
}

void startBleMode() {
  Serial.println("Menginisialisasi Bluetooth (NimBLE)...");

  NimBLEDevice::init(bleName.c_str());

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
    NIMBLE_PROPERTY::READ |
    NIMBLE_PROPERTY::WRITE |
    NIMBLE_PROPERTY::WRITE_NR |   // izinkan write-without-response dari app -> lebih cepat, tidak nunggu ACK
    NIMBLE_PROPERTY::NOTIFY
  );
  pCharacteristic->setCallbacks(new MyCharacteristicCallbacks());
  pService->start();

  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setAppearance(0x0000);

  // Interval advertising lebih rapat -> app menemukan & connect ke device lebih cepat saat scan.
  pAdvertising->setMinInterval(32); // 32 * 0.625ms = 20ms
  pAdvertising->setMaxInterval(48); // 48 * 0.625ms = 30ms

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
    startConfigAP();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(SDA_PIN, SCL_PIN);
  // Fast Mode I2C (400kHz, naik dari default 100kHz) supaya tiap transaksi
  // ke CH224X (setVoltage/getCurrentProfile/isPowerGood/hasProtocol) selesai
  // lebih cepat. CH224A/Q mendukung sampai 400kHz.
  Wire.setClock(400000);

  deviceId = computeDeviceId();
  bleName = "ESP32-Cooler-" + deviceId;

  pinMode(PIN_ONBOARD_LED, OUTPUT);
  onboardLedWrite(false);

  ch224aReady = CH224X1.begin();
  if (!ch224aReady) {
    Serial.println("CH224 not responding! Melanjutkan tanpa kontrol PD (WiFi/BLE tetap aktif).");
    pdStatus = "CH224A_NOT_READY";
  } else {
    CH224X1.setVoltage(0); // mulai aman di 5V
    currentSetVoltage = 5.0;
    pollCH224X(); // baca status awal sekali supaya JSON pertama tidak kosong
  }

  strip.begin();
  strip.setBrightness(80);
  strip.show();
  applyLedMode("off");

  prefs.begin("cooler", false);
  netMode = prefs.getString("netMode", "ble");
  savedSsid = prefs.getString("ssid", "");
  savedPass = prefs.getString("pass", "");

  registerHttpHandlers();

  if (netMode == "wifi" && savedSsid.length() > 0) {
    startWifiControlMode(savedSsid, savedPass);
  } else {
    startBleMode();
    if (savedSsid.length() == 0) {
      startConfigAP();
    } else {
      WiFi.mode(WIFI_OFF);
    }
  }

  startMillis = millis();
}

void loop() {
  // ---- 1. Ambil command BLE pending. Critical section DIPERSEMPIT supaya
  // cuma menyalin buffer kecil (tanpa I2C/tanpa kerja berat di dalamnya) -
  // ini kunci utama supaya BLE stack tidak pernah ketahan lama. ----
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

  bool commandHandled = false;
  if (cmd.length() > 0) {
    processCommandJson(cmd);
    commandHandled = true;

    JsonDocument doc;
    if (!deserializeJson(doc, cmd) && doc["action"].is<const char*>()) {
      handleBleAction(doc["action"].as<String>());
    }
  }

  // ---- 2. Poll I2C ke CH224X di luar critical section & throttled ----
  if (ch224aReady && millis() - lastCh224Poll >= CH224_POLL_INTERVAL_MS) {
    lastCh224Poll = millis();
    pollCH224X();
  }

  if (configApActive || wifiControlActive) {
    server.handleClient();
  }

  if (wifiControlActive && millis() - lastBeacon > 2000) {
    sendUdpBeacon();
    lastBeacon = millis();
  }

  handleOnboardBlink();
  handleLedAnimation();

  // Begitu ada command yang baru diproses, langsung kirim status terbaru
  // (jangan tunggu interval publish berikutnya) supaya app dapat konfirmasi
  // hasil perintah secepat mungkin lewat notify BLE.
  if (commandHandled || millis() - lastPublish > 200) {
    publishStatusBLE();
    lastPublish = millis();
  }
}
