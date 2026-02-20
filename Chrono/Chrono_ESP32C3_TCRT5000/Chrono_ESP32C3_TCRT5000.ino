#include <Arduino.h>
#include <Preferences.h>
#include <NimBLEDevice.h>

// ESP32-C3 + TCRT5000 (digital DO)
// BLE protocol compatible with web app (Nordic UART Service):
// - TX notify: sends "LAP\n" and "CFG ...\n"
// - RX write: accepts "GET" and "SET key=value ..."

// -------- Pins --------
// Change to your wiring if needed.
static const int SENSOR_PIN = 2;      // TCRT5000 DO -> GPIO2
static const int LED_PIN = LED_BUILTIN;

// -------- BLE NUS UUIDs --------
static const char *NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
static const char *NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"; // write from app
static const char *NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"; // notify to app

struct Config {
  uint16_t threshold;     // kept for app compatibility (unused in digital mode)
  uint16_t hysteresis;    // kept for app compatibility (unused in digital mode)
  uint16_t debounceMs;    // ignore state changes faster than this
  uint16_t holdoffMs;     // cooldown between triggers
  uint16_t minLapMs;      // minimum lap time allowed
  uint8_t invert;         // 0/1; many TCRT5000 modules are active LOW -> use invert=1
  uint8_t emaAlphaPct;    // kept for app compatibility (unused in digital mode)
  uint16_t sampleMs;      // sampling period for digital sensor polling
};

Config cfg;
Preferences prefs;

NimBLEServer *bleServer = nullptr;
NimBLECharacteristic *txChar = nullptr;
NimBLECharacteristic *rxChar = nullptr;
bool bleConnected = false;
String rxLine;

unsigned long lastChangeMs = 0;
unsigned long lastLapMs = 0;
unsigned long lastSampleMs = 0;
unsigned long lastPrintMs = 0;
bool hasLap = false;

bool sensorState = false; // physical state (non-inverted)
bool detected = false;    // logical state after invert

static void setFactoryDefaults() {
  cfg.threshold = 450;
  cfg.hysteresis = 40;
  cfg.debounceMs = 12;
  cfg.holdoffMs = 120;
  cfg.minLapMs = 1200;
  cfg.invert = 1;
  cfg.emaAlphaPct = 35;
  cfg.sampleMs = 2;
}

static void serialCfg() {
  Serial.print(F("[CFG] thr=")); Serial.print(cfg.threshold);
  Serial.print(F(" hyst=")); Serial.print(cfg.hysteresis);
  Serial.print(F(" debounce=")); Serial.print(cfg.debounceMs);
  Serial.print(F(" holdoff=")); Serial.print(cfg.holdoffMs);
  Serial.print(F(" minlap=")); Serial.print(cfg.minLapMs);
  Serial.print(F(" invert=")); Serial.print(cfg.invert);
  Serial.print(F(" alpha=")); Serial.print(cfg.emaAlphaPct);
  Serial.print(F(" sample=")); Serial.println(cfg.sampleMs);
}

static void saveConfig() {
  prefs.begin("chrono", false);
  prefs.putUShort("thr", cfg.threshold);
  prefs.putUShort("hyst", cfg.hysteresis);
  prefs.putUShort("deb", cfg.debounceMs);
  prefs.putUShort("hold", cfg.holdoffMs);
  prefs.putUShort("min", cfg.minLapMs);
  prefs.putUChar("inv", cfg.invert);
  prefs.putUChar("alpha", cfg.emaAlphaPct);
  prefs.putUShort("smp", cfg.sampleMs);
  prefs.putBool("has", true);
  prefs.end();
  Serial.println(F("[NVS] Config saved"));
  serialCfg();
}

static void loadConfig() {
  prefs.begin("chrono", true);
  bool has = prefs.getBool("has", false);
  if (!has) {
    prefs.end();
    setFactoryDefaults();
    saveConfig();
    Serial.println(F("[NVS] No valid config, defaults written"));
    return;
  }

  cfg.threshold = prefs.getUShort("thr", 450);
  cfg.hysteresis = prefs.getUShort("hyst", 40);
  cfg.debounceMs = prefs.getUShort("deb", 12);
  cfg.holdoffMs = prefs.getUShort("hold", 120);
  cfg.minLapMs = prefs.getUShort("min", 1200);
  cfg.invert = prefs.getUChar("inv", 1);
  cfg.emaAlphaPct = prefs.getUChar("alpha", 35);
  cfg.sampleMs = prefs.getUShort("smp", 2);
  prefs.end();
  Serial.println(F("[NVS] Config loaded"));
  serialCfg();
}

static String cfgLine() {
  char buf[180];
  snprintf(buf, sizeof(buf),
           "CFG thr=%u hyst=%u debounce=%u holdoff=%u minlap=%u invert=%u alpha=%u sample=%u\n",
           cfg.threshold, cfg.hysteresis, cfg.debounceMs, cfg.holdoffMs,
           cfg.minLapMs, cfg.invert, cfg.emaAlphaPct, cfg.sampleMs);
  return String(buf);
}

static void bleNotifyText(const String &s) {
  if (!txChar || !bleConnected) return;
  txChar->setValue((uint8_t *)s.c_str(), s.length());
  txChar->notify();
}

static bool parseKeyU32(const String &line, const char *key, uint32_t &out) {
  int idx = line.indexOf(key);
  if (idx < 0) return false;
  idx += strlen(key);
  int end = idx;
  while (end < (int)line.length() && isDigit(line[end])) end++;
  if (end <= idx) return false;
  out = (uint32_t)line.substring(idx, end).toInt();
  return true;
}

static void applyCommand(String line) {
  line.trim();
  if (!line.length()) return;

  Serial.print(F("[BLE RX] ")); Serial.println(line);

  String upper = line;
  upper.toUpperCase();

  if (upper.startsWith("GET")) {
    bleNotifyText(cfgLine());
    return;
  }

  if (upper.startsWith("SET")) {
    uint32_t v;
    bool changed = false;
    if (parseKeyU32(line, "thr=", v))      { cfg.threshold = constrain(v, 0, 1023); changed = true; }
    if (parseKeyU32(line, "hyst=", v))     { cfg.hysteresis = constrain(v, 0, 512); changed = true; }
    if (parseKeyU32(line, "debounce=", v)) { cfg.debounceMs = constrain(v, 0, 2000); changed = true; }
    if (parseKeyU32(line, "holdoff=", v))  { cfg.holdoffMs = constrain(v, 0, 10000); changed = true; }
    if (parseKeyU32(line, "minlap=", v))   { cfg.minLapMs = constrain(v, 0, 60000); changed = true; }
    if (parseKeyU32(line, "invert=", v))   { cfg.invert = v ? 1 : 0; changed = true; }
    if (parseKeyU32(line, "alpha=", v))    { cfg.emaAlphaPct = constrain(v, 1, 100); changed = true; }
    if (parseKeyU32(line, "sample=", v))   { cfg.sampleMs = constrain(v, 1, 200); changed = true; }

    if (changed) saveConfig();
    bleNotifyText(cfgLine());
    return;
  }
}

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *pServer) override {
    bleConnected = true;
    Serial.println(F("[BLE] Client connected"));
  }

  void onDisconnect(NimBLEServer *pServer) override {
    bleConnected = false;
    Serial.println(F("[BLE] Client disconnected"));
    NimBLEDevice::startAdvertising();
  }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pCharacteristic) override {
    std::string value = pCharacteristic->getValue();
    for (size_t i = 0; i < value.size(); i++) {
      char c = (char)value[i];
      if (c == '\n') {
        applyCommand(rxLine);
        rxLine = "";
      } else if (c != '\r') {
        rxLine += c;
        if (rxLine.length() > 160) rxLine = "";
      }
    }
  }
};

static void setupBle() {
  NimBLEDevice::init("ScalexLap");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  NimBLEService *svc = bleServer->createService(NUS_SERVICE_UUID);

  txChar = svc->createCharacteristic(
    NUS_TX_UUID,
    NIMBLE_PROPERTY::NOTIFY
  );

  rxChar = svc->createCharacteristic(
    NUS_RX_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  rxChar->setCallbacks(new RxCallbacks());

  svc->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  adv->start();

  Serial.println(F("[BLE] Advertising as ScalexLap"));
}

void setup() {
  pinMode(SENSOR_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("=== ScalexLap ESP32-C3 + TCRT5000 (DO) ==="));

  loadConfig();

  bool rawDigital = digitalRead(SENSOR_PIN) == HIGH;
  sensorState = rawDigital;
  detected = cfg.invert ? !sensorState : sensorState;

  lastLapMs = 0;
  lastChangeMs = millis();
  lastSampleMs = millis();
  lastPrintMs = 0;
  hasLap = false;

  Serial.print(F("[SENSOR] Initial raw=")); Serial.print(rawDigital ? 1 : 0);
  Serial.print(F(" sensor=")); Serial.print(sensorState ? F("ON") : F("OFF"));
  Serial.print(F(" detect=")); Serial.println(detected ? F("ON") : F("OFF"));

  setupBle();
  bleNotifyText(cfgLine());
}

void loop() {
  unsigned long t = millis();

  if (t - lastSampleMs >= cfg.sampleMs) {
    lastSampleMs = t;

    bool rawDigital = digitalRead(SENSOR_PIN) == HIGH;
    bool newSensorState = rawDigital;

    if (newSensorState != sensorState) {
      if (t - lastChangeMs >= cfg.debounceMs) {
        bool prevSensor = sensorState;
        bool prevDetect = detected;

        sensorState = newSensorState;
        detected = cfg.invert ? !sensorState : sensorState;
        lastChangeMs = t;

        Serial.print(F("[STATE] sensor ")); Serial.print(prevSensor ? F("ON") : F("OFF"));
        Serial.print(F(" -> ")); Serial.print(sensorState ? F("ON") : F("OFF"));
        Serial.print(F(" | detect ")); Serial.print(prevDetect ? F("ON") : F("OFF"));
        Serial.print(F(" -> ")); Serial.println(detected ? F("ON") : F("OFF"));

        if (!prevDetect && detected) {
          unsigned long sinceLastLap = t - lastLapMs;

          if (hasLap && sinceLastLap < cfg.holdoffMs) {
            Serial.print(F("[FILTER] holdoff ")); Serial.println(sinceLastLap);
          } else if (hasLap && cfg.minLapMs > 0 && sinceLastLap < cfg.minLapMs) {
            Serial.print(F("[FILTER] minlap ")); Serial.println(sinceLastLap);
          } else {
            if (!hasLap) Serial.println(F("[LAP] First trigger accepted"));
            hasLap = true;
            lastLapMs = t;

            bleNotifyText("LAP\n");
            digitalWrite(LED_PIN, !digitalRead(LED_PIN));

            Serial.print(F("[LAP] OK dt=")); Serial.println(sinceLastLap);
          }
        }
      }
    }

    if (t - lastPrintMs >= 500) {
      lastPrintMs = t;
      Serial.print(F("[HB] raw=")); Serial.print(rawDigital ? 1 : 0);
      Serial.print(F(" sensor=")); Serial.print(sensorState ? F("ON") : F("OFF"));
      Serial.print(F(" detect=")); Serial.print(detected ? F("ON") : F("OFF"));
      Serial.print(F(" hasLap=")); Serial.print(hasLap ? 1 : 0);
      Serial.print(F(" dtSinceLap=")); Serial.print(hasLap ? (t - lastLapMs) : 0);
      Serial.print(F(" debounce=")); Serial.print(cfg.debounceMs);
      Serial.print(F(" holdoff=")); Serial.print(cfg.holdoffMs);
      Serial.print(F(" minlap=")); Serial.print(cfg.minLapMs);
      Serial.print(F(" invert=")); Serial.println(cfg.invert);
    }
  }
}
