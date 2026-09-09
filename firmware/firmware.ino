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
bool authPassSentThisSession = false; // dipakai publishStatusBLE(), direset tiap konek baru

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

uint32_t wheelColor(byte pos);

const uint32_t PALETTE_FIRE[]      = {0xFF0000, 0xFF4500, 0xFFA500, 0xFFFF00};
const uint32_t PALETTE_ICE[]       = {0x001133, 0x0066CC, 0x66CCFF, 0xFFFFFF};
const uint32_t PALETTE_OCEAN[]     = {0x001F3F, 0x0074D9, 0x39CCCC, 0x7FDBFF};
const uint32_t PALETTE_FOREST[]    = {0x013220, 0x228B22, 0x6B8E23, 0x9ACD32};
const uint32_t PALETTE_SUNSET[]    = {0xFF4500, 0xFF6347, 0xFFD700, 0xFF1493};
const uint32_t PALETTE_PARTY[]     = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFF00FF, 0x00FFFF};
const uint32_t PALETTE_RED[]       = {0xFF0000, 0x800000};
const uint32_t PALETTE_GREEN[]     = {0x00FF00, 0x006400};
const uint32_t PALETTE_BLUE[]      = {0x0000FF, 0x000080};
const uint32_t PALETTE_PURPLE[]    = {0x800080, 0x4B0082, 0xEE82EE};
const uint32_t PALETTE_WARMWHITE[] = {0xFFE4B5, 0xFFDAB9, 0xFFFFFF};
const uint32_t PALETTE_COOLWHITE[] = {0xE0FFFF, 0xFFFFFF, 0xADD8E6};
const uint32_t PALETTE_PASTEL[]    = {0xFFD1DC, 0xE6E6FA, 0xC1FFC1, 0xB0E0E6};
const uint32_t PALETTE_REDBLUE[]   = {0xFF0000, 0x0000FF};
const uint32_t PALETTE_PINKCYAN[]  = {0xFF1493, 0x00FFFF};
const uint32_t PALETTE_GOLD[]      = {0xFFD700, 0xB8860B};
const uint32_t PALETTE_NEON[]      = {0x39FF14, 0xFF073A, 0x04D9FF, 0xFE01B1};
const uint32_t PALETTE_CANDY[]     = {0xFF69B4, 0xFFB6C1, 0xFFFFFF};
const uint32_t PALETTE_MONO[]      = {0xFFFFFF};

struct PaletteInfo { const uint32_t* colors; uint8_t count; };
const PaletteInfo PALETTES[] = {
  { nullptr,             0 },
  { PALETTE_FIRE,        4 },
  { PALETTE_ICE,         4 },
  { PALETTE_OCEAN,       4 },
  { PALETTE_FOREST,      4 },
  { PALETTE_SUNSET,      4 },
  { PALETTE_PARTY,       6 },
  { PALETTE_RED,         2 },
  { PALETTE_GREEN,       2 },
  { PALETTE_BLUE,        2 },
  { PALETTE_PURPLE,      3 },
  { PALETTE_WARMWHITE,   3 },
  { PALETTE_COOLWHITE,   3 },
  { PALETTE_PASTEL,      4 },
  { PALETTE_REDBLUE,     2 },
  { PALETTE_PINKCYAN,    2 },
  { PALETTE_GOLD,        2 },
  { PALETTE_NEON,        4 },
  { PALETTE_CANDY,       3 },
  { PALETTE_MONO,        1 },
};

uint32_t paletteColor(int paletteIdx, uint8_t pos) {
  if (paletteIdx <= 0 || paletteIdx >= (int)(sizeof(PALETTES) / sizeof(PALETTES[0]))) {
    return wheelColor(pos);
  }
  const PaletteInfo& pal = PALETTES[paletteIdx];
  if (pal.count == 0) return wheelColor(pos);
  if (pal.count == 1) return pal.colors[0];

  float scaled = (pos / 256.0f) * pal.count;
  int idxA = ((int)scaled) % pal.count;
  int idxB = (idxA + 1) % pal.count;
  float frac = scaled - (int)scaled;

  uint32_t ca = pal.colors[idxA];
  uint32_t cb = pal.colors[idxB];
  uint8_t ra = (ca >> 16) & 0xFF, ga = (ca >> 8) & 0xFF, ba = ca & 0xFF;
  uint8_t rb = (cb >> 16) & 0xFF, gb = (cb >> 8) & 0xFF, bb = cb & 0xFF;
  uint8_t r = ra + (rb - ra) * frac;
  uint8_t g = ga + (gb - ga) * frac;
  uint8_t b = ba + (bb - ba) * frac;
  return strip.Color(r, g, b);
}

struct LedModeInfo { const char* id; const char* label; int8_t engine; int8_t palette; uint16_t speedMs; };
const LedModeInfo LED_MODES[] = {
  {"off", "Off", -1, 0, 0},
  {"static_rainbow", "Static - Rainbow", 0, 0, 60},
  {"static_fire", "Static - Fire", 0, 1, 60},
  {"static_ice", "Static - Ice", 0, 2, 60},
  {"static_ocean", "Static - Ocean", 0, 3, 60},
  {"static_forest", "Static - Forest", 0, 4, 60},
  {"static_sunset", "Static - Sunset", 0, 5, 60},
  {"static_party", "Static - Party", 0, 6, 60},
  {"static_red", "Static - Red", 0, 7, 60},
  {"static_green", "Static - Green", 0, 8, 60},
  {"static_blue", "Static - Blue", 0, 9, 60},
  {"static_purple", "Static - Purple", 0, 10, 60},
  {"static_warmwhite", "Static - Warm White", 0, 11, 60},
  {"static_coolwhite", "Static - Cool White", 0, 12, 60},
  {"static_pastel", "Static - Pastel", 0, 13, 60},
  {"static_redblue", "Static - Red-Blue", 0, 14, 60},
  {"static_pinkcyan", "Static - Pink-Cyan", 0, 15, 60},
  {"static_gold", "Static - Gold", 0, 16, 60},
  {"static_neon", "Static - Neon", 0, 17, 60},
  {"static_candy", "Static - Candy", 0, 18, 60},
  {"static_mono", "Static - Mono White", 0, 19, 60},
  {"rainbowrun_rainbow", "Rainbow Run - Rainbow", 1, 0, 25},
  {"rainbowrun_fire", "Rainbow Run - Fire", 1, 1, 25},
  {"rainbowrun_ice", "Rainbow Run - Ice", 1, 2, 25},
  {"rainbowrun_ocean", "Rainbow Run - Ocean", 1, 3, 25},
  {"rainbowrun_forest", "Rainbow Run - Forest", 1, 4, 25},
  {"rainbowrun_sunset", "Rainbow Run - Sunset", 1, 5, 25},
  {"rainbowrun_party", "Rainbow Run - Party", 1, 6, 25},
  {"rainbowrun_red", "Rainbow Run - Red", 1, 7, 25},
  {"rainbowrun_green", "Rainbow Run - Green", 1, 8, 25},
  {"rainbowrun_blue", "Rainbow Run - Blue", 1, 9, 25},
  {"rainbowrun_purple", "Rainbow Run - Purple", 1, 10, 25},
  {"rainbowrun_warmwhite", "Rainbow Run - Warm White", 1, 11, 25},
  {"rainbowrun_coolwhite", "Rainbow Run - Cool White", 1, 12, 25},
  {"rainbowrun_pastel", "Rainbow Run - Pastel", 1, 13, 25},
  {"rainbowrun_redblue", "Rainbow Run - Red-Blue", 1, 14, 25},
  {"rainbowrun_pinkcyan", "Rainbow Run - Pink-Cyan", 1, 15, 25},
  {"rainbowrun_gold", "Rainbow Run - Gold", 1, 16, 25},
  {"rainbowrun_neon", "Rainbow Run - Neon", 1, 17, 25},
  {"rainbowrun_candy", "Rainbow Run - Candy", 1, 18, 25},
  {"rainbowrun_mono", "Rainbow Run - Mono White", 1, 19, 25},
  {"rainbowcycle_rainbow", "Rainbow Cycle - Rainbow", 2, 0, 20},
  {"rainbowcycle_fire", "Rainbow Cycle - Fire", 2, 1, 20},
  {"rainbowcycle_ice", "Rainbow Cycle - Ice", 2, 2, 20},
  {"rainbowcycle_ocean", "Rainbow Cycle - Ocean", 2, 3, 20},
  {"rainbowcycle_forest", "Rainbow Cycle - Forest", 2, 4, 20},
  {"rainbowcycle_sunset", "Rainbow Cycle - Sunset", 2, 5, 20},
  {"rainbowcycle_party", "Rainbow Cycle - Party", 2, 6, 20},
  {"rainbowcycle_red", "Rainbow Cycle - Red", 2, 7, 20},
  {"rainbowcycle_green", "Rainbow Cycle - Green", 2, 8, 20},
  {"rainbowcycle_blue", "Rainbow Cycle - Blue", 2, 9, 20},
  {"rainbowcycle_purple", "Rainbow Cycle - Purple", 2, 10, 20},
  {"rainbowcycle_warmwhite", "Rainbow Cycle - Warm White", 2, 11, 20},
  {"rainbowcycle_coolwhite", "Rainbow Cycle - Cool White", 2, 12, 20},
  {"rainbowcycle_pastel", "Rainbow Cycle - Pastel", 2, 13, 20},
  {"rainbowcycle_redblue", "Rainbow Cycle - Red-Blue", 2, 14, 20},
  {"rainbowcycle_pinkcyan", "Rainbow Cycle - Pink-Cyan", 2, 15, 20},
  {"rainbowcycle_gold", "Rainbow Cycle - Gold", 2, 16, 20},
  {"rainbowcycle_neon", "Rainbow Cycle - Neon", 2, 17, 20},
  {"rainbowcycle_candy", "Rainbow Cycle - Candy", 2, 18, 20},
  {"rainbowcycle_mono", "Rainbow Cycle - Mono White", 2, 19, 20},
  {"disco_rainbow", "Disco - Rainbow", 3, 0, 50},
  {"disco_fire", "Disco - Fire", 3, 1, 50},
  {"disco_ice", "Disco - Ice", 3, 2, 50},
  {"disco_ocean", "Disco - Ocean", 3, 3, 50},
  {"disco_forest", "Disco - Forest", 3, 4, 50},
  {"disco_sunset", "Disco - Sunset", 3, 5, 50},
  {"disco_party", "Disco - Party", 3, 6, 50},
  {"disco_red", "Disco - Red", 3, 7, 50},
  {"disco_green", "Disco - Green", 3, 8, 50},
  {"disco_blue", "Disco - Blue", 3, 9, 50},
  {"disco_purple", "Disco - Purple", 3, 10, 50},
  {"disco_warmwhite", "Disco - Warm White", 3, 11, 50},
  {"disco_coolwhite", "Disco - Cool White", 3, 12, 50},
  {"disco_pastel", "Disco - Pastel", 3, 13, 50},
  {"disco_redblue", "Disco - Red-Blue", 3, 14, 50},
  {"disco_pinkcyan", "Disco - Pink-Cyan", 3, 15, 50},
  {"disco_gold", "Disco - Gold", 3, 16, 50},
  {"disco_neon", "Disco - Neon", 3, 17, 50},
  {"disco_candy", "Disco - Candy", 3, 18, 50},
  {"disco_mono", "Disco - Mono White", 3, 19, 50},
  {"confetti_rainbow", "Confetti - Rainbow", 4, 0, 30},
  {"confetti_fire", "Confetti - Fire", 4, 1, 30},
  {"confetti_ice", "Confetti - Ice", 4, 2, 30},
  {"confetti_ocean", "Confetti - Ocean", 4, 3, 30},
  {"confetti_forest", "Confetti - Forest", 4, 4, 30},
  {"confetti_sunset", "Confetti - Sunset", 4, 5, 30},
  {"confetti_party", "Confetti - Party", 4, 6, 30},
  {"confetti_red", "Confetti - Red", 4, 7, 30},
  {"confetti_green", "Confetti - Green", 4, 8, 30},
  {"confetti_blue", "Confetti - Blue", 4, 9, 30},
  {"confetti_purple", "Confetti - Purple", 4, 10, 30},
  {"confetti_warmwhite", "Confetti - Warm White", 4, 11, 30},
  {"confetti_coolwhite", "Confetti - Cool White", 4, 12, 30},
  {"confetti_pastel", "Confetti - Pastel", 4, 13, 30},
  {"confetti_redblue", "Confetti - Red-Blue", 4, 14, 30},
  {"confetti_pinkcyan", "Confetti - Pink-Cyan", 4, 15, 30},
  {"confetti_gold", "Confetti - Gold", 4, 16, 30},
  {"confetti_neon", "Confetti - Neon", 4, 17, 30},
  {"confetti_candy", "Confetti - Candy", 4, 18, 30},
};
const int NUM_LED_MODES = sizeof(LED_MODES) / sizeof(LED_MODES[0]);

int findLedModeIndex(const String& id) {
  for (int i = 0; i < NUM_LED_MODES; i++) {
    if (id == LED_MODES[i].id) return i;
  }
  return -1;
}

String buildLedModesJson() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < NUM_LED_MODES; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = LED_MODES[i].id;
    o["label"] = LED_MODES[i].label;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

String ledMode = "off";
String lastLedEffect = "rainbowrun_rainbow";
int currentLedModeIdx = 0;
int8_t currentEngine = -1;
int8_t currentPalette = 0;
uint16_t currentSpeedMs = 30;

unsigned long lastLedStep = 0;
uint16_t animStep = 0;
uint8_t confettiFade[NUM_LEDS];

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
String pdStatus = "CH224A_NOT_READY";

void updatePdStatus() {
  if (!ch224aReady) {
    pdStatus = "CH224A_NOT_READY";
  } else if (pgood) {
    pdStatus = "PD_NEGOTIATED";
  } else {
    pdStatus = "PD_WAITING";
  }
}

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
    authPassSentThisSession = false;
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
  int idx = findLedModeIndex(mode);
  if (idx < 0) return;

  currentLedModeIdx = idx;
  ledMode = mode;
  currentEngine = LED_MODES[idx].engine;
  currentPalette = LED_MODES[idx].palette;
  currentSpeedMs = LED_MODES[idx].speedMs;
  if (mode != "off") lastLedEffect = mode;

  animStep = 0;
  lastLedStep = 0;
  memset(confettiFade, 0, sizeof(confettiFade));

  if (currentEngine == -1) {
    strip.clear();
    strip.show();
  } else if (currentEngine == 0) {
    for (int i = 0; i < NUM_LEDS; i++) {
      int hue = (i * 256 / NUM_LEDS) & 255;
      strip.setPixelColor(i, paletteColor(currentPalette, hue));
    }
    strip.show();
  }

  if (prefs.getString("ledMode", "") != ledMode) {
    prefs.putString("ledMode", ledMode);
  }
}

void handleLedAnimation() {
  if (currentEngine < 1) return;
  if (millis() - lastLedStep < currentSpeedMs) return;
  lastLedStep = millis();

  switch (currentEngine) {
    case 1: {
      for (int i = 0; i < NUM_LEDS; i++) {
        int hue = ((i * 256 / NUM_LEDS) + animStep) & 255;
        strip.setPixelColor(i, paletteColor(currentPalette, hue));
      }
      strip.show();
      animStep = (animStep + 3) & 255;
      break;
    }
    case 2: {
      uint32_t c = paletteColor(currentPalette, animStep & 255);
      for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, c);
      strip.show();
      animStep = (animStep + 2) & 255;
      break;
    }
    case 3: {
      for (int i = 0; i < NUM_LEDS; i++) {
        strip.setPixelColor(i, paletteColor(currentPalette, random(0, 256)));
      }
      strip.show();
      break;
    }
    case 4: {
      for (int i = 0; i < NUM_LEDS; i++) {
        confettiFade[i] = (confettiFade[i] > 12) ? confettiFade[i] - 12 : 0;
      }
      if (random(0, 10) < 6) {
        int p = random(0, NUM_LEDS);
        confettiFade[p] = 255;
      }
      for (int i = 0; i < NUM_LEDS; i++) {
        uint32_t c = paletteColor(currentPalette, (i * 40) & 255);
        uint8_t r = ((c >> 16) & 0xFF) * confettiFade[i] / 255;
        uint8_t g = ((c >> 8) & 0xFF) * confettiFade[i] / 255;
        uint8_t b = (c & 0xFF) * confettiFade[i] / 255;
        strip.setPixelColor(i, strip.Color(r, g, b));
      }
      strip.show();
      break;
    }
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
  doc["pdStatus"] = pdStatus;
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

// BLE GATT notify TIDAK otomatis dipotong-sambung kalau datanya lebih
// panjang dari MTU (beda sama operasi "read", yang memang auto-reassembly).
// JSON status ini sudah lumayan panjang (pdStatus, fanRpm, netMode,
// wifiConnected, httpAuthPass, dll) - kalau dikirim mentah lewat satu kali
// notify() dan lebih panjang dari (MTU-3) byte, sisanya kepotong diam-diam
// dan hasilnya JSON rusak di sisi app (gagal di-parse, SEMUA field jadi
// gak keupdate - persis gejala "macet di 5V, PD gak kebaca").
//
// Solusinya: pecah jadi beberapa notify kecil, masing-masing diawali 2
// byte header (index chunk, total chunk), app yang nyambung ulang. Ini
// jauh lebih aman daripada cuma ngirit field, karena JSON pasti bakal
// nambah panjang lagi ke depannya kalau ada fitur baru.
#define BLE_CHUNK_SIZE 180

void publishStatusBLE() {
  if (!deviceConnected || pCharacteristic == nullptr) return;

  // httpAuthPass cuma perlu dikirim SEKALI per sesi koneksi (app nyimpen
  // begitu dapat), bukan tiap 300ms selamanya - itu buang-buang bandwidth
  // BLE yang udah pas-pasan buat field lain.
  String jsonStr = buildStatusJson(!authPassSentThisSession);
  if (!authPassSentThisSession) authPassSentThisSession = true;

  size_t total = jsonStr.length();
  size_t numChunks = (total + BLE_CHUNK_SIZE - 1) / BLE_CHUNK_SIZE;
  if (numChunks == 0) numChunks = 1;
  if (numChunks > 255) numChunks = 255; // batas 1 byte di header, JSON segini panjang seharusnya gak kejadian

  for (size_t i = 0; i < numChunks; i++) {
    size_t start = i * BLE_CHUNK_SIZE;
    size_t len = min((size_t)BLE_CHUNK_SIZE, total - start);
    uint8_t packet[BLE_CHUNK_SIZE + 2];
    packet[0] = (uint8_t)i;
    packet[1] = (uint8_t)numChunks;
    memcpy(packet + 2, jsonStr.c_str() + start, len);
    pCharacteristic->setValue(packet, len + 2);
    pCharacteristic->notify();
    if (numChunks > 1) delay(15); // kasih jeda kecil antar potongan biar gak ketimpa/ke-drop stack BLE-nya
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
<title>Setup WiFi - Vladimir Putin</title>
<style>
  :root{
    --bg:#05070c; --card:#0d1220; --border:#1c2436; --accent:#22d3ee; --accent2:#a78bfa;
    --text:#eef1fb; --faint:#7b869c; --ok:#34e0a1; --danger:#ff5d6c;
  }
  *{box-sizing:border-box;}
  html,body{margin:0;padding:0;}
  body{
    background:
      radial-gradient(circle at 1px 1px, rgba(255,255,255,.05) 1px, transparent 0) 0 0/22px 22px,
      linear-gradient(160deg,#05070c,#080b16 55%,#05070c);
    color:var(--text); font-family:-apple-system,"Segoe UI",Roboto,Arial,sans-serif;
    padding:22px 16px 40px; min-height:100vh;
  }
  .head{display:flex; align-items:center; gap:12px; margin:4px 0 22px;}
  .head .icon{
    width:42px; height:42px; border-radius:12px; flex-shrink:0;
    background:linear-gradient(135deg, rgba(34,211,238,.15), rgba(167,139,250,.15));
    border:1px solid rgba(34,211,238,.4);
    display:flex; align-items:center; justify-content:center;
    box-shadow:0 0 18px rgba(34,211,238,.25);
  }
  .head .icon svg{width:22px;height:22px;}
  .head h1{
    font-size:16px; margin:0; letter-spacing:1.5px; font-weight:800; text-transform:uppercase;
    background:linear-gradient(90deg,var(--accent),var(--accent2));
    -webkit-background-clip:text; background-clip:text; color:transparent;
  }
  .head .tag{font-size:10.5px; letter-spacing:1px; color:var(--faint); text-transform:uppercase; margin-top:2px;}

  .card{
    position:relative; background:var(--card); border:1px solid var(--border); border-radius:14px;
    padding:18px; margin-bottom:14px; animation:rise .45s ease both;
  }
  .card::before, .card::after{
    content:""; position:absolute; width:14px; height:14px; border:1.5px solid rgba(34,211,238,.55);
    opacity:.8;
  }
  .card::before{ top:-1px; left:-1px; border-right:none; border-bottom:none; border-top-left-radius:8px; }
  .card::after{ bottom:-1px; right:-1px; border-left:none; border-top:none; border-bottom-right-radius:8px; }
  @keyframes rise{ from{opacity:0; transform:translateY(8px);} to{opacity:1; transform:translateY(0);} }

  .label{font-size:10.5px; letter-spacing:1.2px; text-transform:uppercase; color:var(--faint); margin-bottom:10px; display:flex; align-items:center; gap:6px;}
  .label svg{width:13px;height:13px; opacity:.8;}

  .row{display:flex; align-items:center; justify-content:space-between; padding:9px 0; border-bottom:1px dashed rgba(255,255,255,.06);}
  .row:last-child{border-bottom:none;}
  .row .k{color:var(--faint); font-size:12.5px;}
  .row .v{font-family:"SF Mono",Consolas,monospace; font-weight:700; letter-spacing:1.5px; font-size:13.5px; color:var(--accent);}

  .hint{color:var(--faint); font-size:11.5px; line-height:1.6; margin-top:10px; padding-top:10px; border-top:1px dashed rgba(255,255,255,.06);}

  .netrow{display:flex; align-items:center; justify-content:space-between; padding:8px 0;}
  .netrow .k{color:var(--faint); font-size:12px;}
  .netrow .v{font-size:12.5px; font-weight:600;}

  button{
    width:100%; padding:14px; border-radius:12px; border:none; font-size:13px; font-weight:800;
    letter-spacing:.8px; text-transform:uppercase; cursor:pointer; margin-top:8px;
    display:flex; align-items:center; justify-content:center; gap:8px;
    transition:transform .12s ease, filter .12s ease;
  }
  button:active{ transform:scale(.98); filter:brightness(.92); }
  button svg{width:16px;height:16px;}
  .btn-primary{background:linear-gradient(90deg,var(--accent),var(--accent2)); color:#031018; box-shadow:0 6px 20px rgba(34,211,238,.2);}
  .btn-outline{background:transparent; border:1px solid var(--border); color:var(--text);}
  .btn-outline:active{border-color:var(--accent);}

  input{
    width:100%; padding:13px 14px; border-radius:12px; border:1px solid var(--border);
    background:#080c16; color:var(--text); font-size:13.5px; margin-top:10px;
  }
  input:focus{outline:none; border-color:var(--accent); box-shadow:0 0 0 3px rgba(34,211,238,.12);}
  input::placeholder{color:#4a5468;}

  #wifiList{margin-top:10px;}
  .wifi-item{
    display:flex; justify-content:space-between; align-items:center; padding:12px 14px;
    background:#080c16; border:1px solid var(--border); border-radius:10px; margin-bottom:7px; cursor:pointer;
  }
  .wifi-item:active{border-color:var(--accent);}
  .wifi-item .left{display:flex; align-items:center; gap:10px;}
  .wifi-item .ssid{font-size:13px; font-weight:600;}
  .bars{display:flex; align-items:flex-end; gap:2px; height:12px;}
  .bars i{width:3px; background:#2a3448; border-radius:1px;}
  .bars i.on{background:var(--accent);}
  .bars i:nth-child(1){height:4px;} .bars i:nth-child(2){height:7px;}
  .bars i:nth-child(3){height:10px;} .bars i:nth-child(4){height:13px;}

  #status{margin-top:12px; font-size:12.5px; line-height:1.6; display:flex; gap:8px; align-items:flex-start;}
  #status svg{width:15px;height:15px; flex-shrink:0; margin-top:1px;}
  .ok{color:var(--ok);} .err{color:var(--danger);} .faint2{color:var(--faint);}
  .spin{
    display:inline-block; width:14px; height:14px; border:2px solid #2a3448; border-top-color:var(--accent);
    border-radius:50%; animation:sp .7s linear infinite; flex-shrink:0; margin-top:1px;
  }
  @keyframes sp{to{transform:rotate(360deg);}}
</style>
</head>
<body>

  <div class="head">
    <div class="icon">
      <svg viewBox="0 0 24 24" fill="none" stroke="#22d3ee" stroke-width="1.6"><rect x="7" y="7" width="10" height="10" rx="2"/><path d="M9 3v4M15 3v4M9 17v4M15 17v4M3 9h4M3 15h4M17 9h4M17 15h4"/></svg>
    </div>
    <div>
      <h1>Setup WiFi</h1>
      <div class="tag">Cooler Control Node</div>
    </div>
  </div>

  <div class="card" style="animation-delay:.02s">
    <div class="label">
      <svg viewBox="0 0 24 24" fill="none" stroke="#7b869c" stroke-width="1.8"><rect x="4" y="2" width="16" height="20" rx="2"/><path d="M10 18h4"/></svg>
      Identitas Device
    </div>
    <div class="row"><span class="k">Device ID</span><span class="v" id="deviceId">-</span></div>
    <div class="row"><span class="k">Password Kontrol</span><span class="v" id="authPass">-</span></div>
    <div class="hint">Catat 2 nilai di atas. Kalau nanti nambah cooler ini lewat menu "Manual (WiFi)" di aplikasi, kamu perlu masukkan keduanya supaya bisa mengontrol (bukan cuma lihat status).</div>
    <div class="netrow" id="rowNet" style="display:none;">
      <span class="k">Status Jaringan</span><span class="v" id="netInfo">-</span>
    </div>
  </div>

  <div class="card" style="animation-delay:.08s">
    <div class="label">
      <svg viewBox="0 0 24 24" fill="none" stroke="#7b869c" stroke-width="1.8"><path d="M5 12.5a11 11 0 0 1 14 0M8 16a6.5 6.5 0 0 1 8 0M12 19.5v.01"/></svg>
      Sambungkan ke WiFi
    </div>
    <button class="btn-outline" onclick="scanWifi()">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="11" cy="11" r="7"/><path d="M21 21l-4.3-4.3"/></svg>
      Cari WiFi Sekitar
    </button>
    <div id="wifiList"></div>

    <input type="text" id="ssid" placeholder="Nama WiFi (SSID)">
    <input type="password" id="password" placeholder="Password WiFi">
    <button class="btn-primary" onclick="connectWifi()">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M9 12l2 2 4-4M12 3l9 6-9 6-9-6 9-6z"/></svg>
      Hubungkan
    </button>
    <div id="status"></div>
  </div>

<script>
const ICON_OK = '<svg viewBox="0 0 24 24" fill="none" stroke="#34e0a1" stroke-width="2"><circle cx="12" cy="12" r="9"/><path d="M8 12l3 3 5-6"/></svg>';
const ICON_ERR = '<svg viewBox="0 0 24 24" fill="none" stroke="#ff5d6c" stroke-width="2"><circle cx="12" cy="12" r="9"/><path d="M12 8v5M12 16v.01"/></svg>';

function setStatus(html, cls, icon){
  const el = document.getElementById('status');
  el.innerHTML = (icon || '') + '<span>' + html + '</span>';
  el.className = cls || '';
}

function barsHtml(rssi){
  const n = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
  let h = '<div class="bars">';
  for (let i = 1; i <= 4; i++) h += '<i class="' + (i <= n ? 'on' : '') + '"></i>';
  return h + '</div>';
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
  list.innerHTML = '<p style="color:#7b869c;font-size:12px;display:flex;align-items:center;gap:8px;"><span class="spin"></span>Memindai jaringan...</p>';
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
      list.innerHTML = '<p style="color:#ff5d6c;font-size:12px;">Gagal memindai, coba lagi.</p>';
      return;
    }
    if (networks.length === 0){
      list.innerHTML = '<p style="color:#7b869c;font-size:12px;">Tidak ada WiFi ditemukan.</p>';
      return;
    }
    networks.sort((a,b) => b.rssi - a.rssi);
    list.innerHTML = '';
    networks.forEach(n => {
      const div = document.createElement('div');
      div.className = 'wifi-item';
      div.innerHTML = '<span class="left"><span class="ssid">' + n.ssid + '</span></span>' + barsHtml(n.rssi);
      div.onclick = () => { document.getElementById('ssid').value = n.ssid; document.getElementById('password').focus(); };
      list.appendChild(div);
    });
  }catch(e){
    list.innerHTML = '<p style="color:#ff5d6c;font-size:12px;">Gagal memindai, coba lagi.</p>';
  }
}

async function connectWifi(){
  const ssid = document.getElementById('ssid').value.trim();
  const password = document.getElementById('password').value;
  if (!ssid){ setStatus('Isi nama WiFi dulu.', 'err', ICON_ERR); return; }
  setStatus('Menyimpan &amp; menghubungkan...', 'faint2', '<span class="spin"></span>');
  try{
    const body = 'ssid=' + encodeURIComponent(ssid) + '&password=' + encodeURIComponent(password);
    await fetch('/setwifi', {
      method: 'POST',
      headers: {'Content-Type':'application/x-www-form-urlencoded'},
      body: body
    });
    setStatus('Tersimpan! ESP32 sedang restart &amp; mencoba konek ke WiFi rumah (kurang lebih 15 detik).<br>' +
               'Sambungkan HP kembali ke WiFi rumah, lalu buka aplikasi, Tambah Cooler, tab Manual (WiFi), masukkan Device ID di atas.', 'ok', ICON_OK);
  }catch(e){
    setStatus('Perintah terkirim. ESP32 kemungkinan sudah restart untuk konek WiFi (koneksi ke halaman ini terputus, itu normal).<br>' +
               'Sambungkan HP kembali ke WiFi rumah lalu buka aplikasi.', 'ok', ICON_OK);
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
  server.send(200, "application/json", buildStatusJson(configApActive));
}

void handleLedModesHttp() {
  lastAppContact = millis();
  server.send(200, "application/json", buildLedModesJson());
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
  server.send(200, "application/json", buildStatusJson(false));
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
  server.on("/ledmodes", HTTP_GET, handleLedModesHttp);
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
    WiFi.setSleep(false);
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
  updatePdStatus();

  strip.begin();
  strip.setBrightness(80);
  strip.show();
  applyLedMode(savedLedMode);

  registerHttpHandlers();

  if (netMode == "wifi" && savedSsid.length() > 0) {
    startWifiControlMode(savedSsid, savedPass);
  } else {
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
      updatePdStatus();
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
    updatePdStatus();
  }

  if (millis() - lastPublish > 300) {
    publishStatusBLE();
    lastPublish = millis();
  }
}
