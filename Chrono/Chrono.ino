#include <Arduino.h>
#include <EEPROM.h>
#include <Adafruit_BluefruitLE_SPI.h>

// =====================
// Bluefruit (SPI) pins - Feather 32u4 Bluefruit LE
// =====================
#define BLUEFRUIT_SPI_CS   8
#define BLUEFRUIT_SPI_IRQ  7
#define BLUEFRUIT_SPI_RST  4
Adafruit_BluefruitLE_SPI ble(BLUEFRUIT_SPI_CS, BLUEFRUIT_SPI_IRQ, BLUEFRUIT_SPI_RST);

// =====================
// Sharp GP2Y0A21YK0F (ANALOG)
// =====================
#define SENSOR_PIN A0
#define LED_PIN    13

// =====================
// Persistent config
// =====================
struct Config {
  uint16_t threshold;     // ADC threshold (0..1023) for "detect"
  uint16_t hysteresis;    // ADC hysteresis (0..512)
  uint16_t debounceMs;    // ignore state changes faster than this
  uint16_t holdoffMs;     // cooldown between triggers
  uint16_t minLapMs;      // minimum lap time allowed
  uint8_t  invert;        // 0/1 (invert detect logic)
  uint8_t  emaAlphaPct;   // EMA alpha in % (1..100). higher = less smoothing
  uint16_t sampleMs;      // sampling period for analog processing
  uint32_t magic;
};
const uint32_t CFG_MAGIC = 0x5343414CUL; // "SCAL"
Config cfg;

// =====================
// Runtime state
// =====================
unsigned long lastChangeMs = 0;
unsigned long lastLapMs    = 0;
unsigned long lastSampleMs = 0;
unsigned long lastPrintMs  = 0;

bool detected = false;    // current detect state (after hysteresis)
float filt = 0.0f;        // filtered analog value (EMA)
String rxLine;

// =====================
// Helpers
// =====================
static void serialCfg() {
  Serial.print(F("[CFG] threshold=")); Serial.print(cfg.threshold);
  Serial.print(F(" hysteresis="));     Serial.print(cfg.hysteresis);
  Serial.print(F(" debounceMs="));     Serial.print(cfg.debounceMs);
  Serial.print(F(" holdoffMs="));      Serial.print(cfg.holdoffMs);
  Serial.print(F(" minLapMs="));       Serial.print(cfg.minLapMs);
  Serial.print(F(" invert="));         Serial.print(cfg.invert);
  Serial.print(F(" emaAlphaPct="));    Serial.print(cfg.emaAlphaPct);
  Serial.print(F(" sampleMs="));       Serial.println(cfg.sampleMs);
}

static void bleSendCfgLine() {
  char buf[180];
  snprintf(buf, sizeof(buf),
    "CFG thr=%u hyst=%u debounce=%u holdoff=%u minlap=%u invert=%u alpha=%u sample=%u\n",
    cfg.threshold, cfg.hysteresis, cfg.debounceMs, cfg.holdoffMs, cfg.minLapMs,
    cfg.invert, cfg.emaAlphaPct, cfg.sampleMs);
  ble.print(buf);
}

static void loadConfig() {
  EEPROM.get(0, cfg);
  if (cfg.magic != CFG_MAGIC) {
    // Defaults that usually work as a starting point
    cfg.threshold   = 450;   // adjust after seeing [HB] logs
    cfg.hysteresis  = 40;
    cfg.debounceMs  = 30;
    cfg.holdoffMs   = 250;
    cfg.minLapMs    = 15000;
    cfg.invert      = 0;
    cfg.emaAlphaPct = 35;    // 35% new, 65% old (moderate smoothing)
    cfg.sampleMs    = 15;    // ~66Hz sampling
    cfg.magic       = CFG_MAGIC;
    EEPROM.put(0, cfg);
    Serial.println(F("[EEPROM] No valid config found -> defaults saved."));
  } else {
    Serial.println(F("[EEPROM] Config loaded."));
  }
  serialCfg();
}

static void saveConfig() {
  cfg.magic = CFG_MAGIC;
  EEPROM.put(0, cfg);
  Serial.println(F("[EEPROM] Config saved."));
  serialCfg();
}

static void applyCommand(String line) {
  line.trim();
  if (!line.length()) return;

  Serial.print(F("[BLE RX] ")); Serial.println(line);

  String upper = line; upper.toUpperCase();

  if (upper.startsWith("GET")) {
    Serial.println(F("[CMD] GET -> send CFG"));
    bleSendCfgLine();
    return;
  }

  if (upper.startsWith("SET")) {
    auto readInt = [&](const char* key, uint32_t& out)->bool {
      int idx = line.indexOf(key);
      if (idx < 0) return false;
      idx += strlen(key);
      int end = idx;
      while (end < (int)line.length() && isDigit(line[end])) end++;
      out = (uint32_t) line.substring(idx, end).toInt();
      return true;
    };

    uint32_t v;
    bool changed = false;

    // SET thr=450 hyst=40 debounce=30 holdoff=250 minlap=15000 invert=0 alpha=35 sample=15
    if (readInt("thr=", v))      { cfg.threshold   = (uint16_t)constrain(v, 0, 1023); changed = true; }
    if (readInt("hyst=", v))     { cfg.hysteresis  = (uint16_t)constrain(v, 0, 512);  changed = true; }
    if (readInt("debounce=", v)) { cfg.debounceMs  = (uint16_t)constrain(v, 0, 2000); changed = true; }
    if (readInt("holdoff=", v))  { cfg.holdoffMs   = (uint16_t)constrain(v, 0, 10000);changed = true; }
    if (readInt("minlap=", v))   { cfg.minLapMs    = (uint16_t)constrain(v, 0, 60000);changed = true; }
    if (readInt("invert=", v))   { cfg.invert      = (uint8_t)(v ? 1 : 0);            changed = true; }
    if (readInt("alpha=", v))    { cfg.emaAlphaPct = (uint8_t)constrain(v, 1, 100);   changed = true; }
    if (readInt("sample=", v))   { cfg.sampleMs    = (uint16_t)constrain(v, 1, 200);  changed = true; }

    if (changed) {
      Serial.println(F("[CMD] SET applied."));
      saveConfig();
    } else {
      Serial.println(F("[CMD] SET received but no valid keys found."));
    }

    bleSendCfgLine();
    return;
  }

  Serial.println(F("[CMD] Unknown command (use GET or SET ...)"));
}

static void bleFatal() {
  Serial.println(F("[BLE] begin() failed. Stopping."));
  while (1) delay(1000);
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  pinMode(SENSOR_PIN, INPUT);

  Serial.begin(115200);
  while (!Serial) delay(10);
  Serial.println();
  Serial.println(F("=== ScalexLap (ANALOG Sharp GP2Y0A21YK0F) ==="));

  loadConfig();

  // init filter with first read
  int r0 = analogRead(SENSOR_PIN);
  filt = (float)r0;

  Serial.println(F("[BLE] Starting..."));
  if (!ble.begin(false)) bleFatal();

  Serial.println(F("[BLE] factoryReset()..."));
  ble.factoryReset();
  ble.echo(false);

  Serial.println(F("[BLE] Setting device name: ScalexLap"));
  ble.println(F("AT+GAPDEVNAME=ScalexLap"));
  ble.waitForOK();

  ble.setMode(BLUEFRUIT_MODE_DATA);

  lastLapMs = millis();
  lastChangeMs = millis();
  lastSampleMs = millis();
  lastPrintMs = 0;

  Serial.print(F("[SENSOR] Initial raw=")); Serial.print(r0);
  Serial.print(F(" filt=")); Serial.println((int)filt);

  Serial.println(F("[BLE] Ready. Waiting for connections..."));
  Serial.println(F("[TIP] Ajusta thr/hyst mirando [HB]. Si se detecta al revés usa invert=1."));
  bleSendCfgLine();
}

void loop() {
  // =====================
  // BLE RX
  // =====================
  while (ble.available()) {
    char c = (char)ble.read();
    if (c == '\n') {
      applyCommand(rxLine);
      rxLine = "";
    } else if (c != '\r') {
      rxLine += c;
      if (rxLine.length() > 120) {
        Serial.println(F("[BLE RX] line too long -> cleared"));
        rxLine = "";
      }
    }
  }

  unsigned long t = millis();

  // =====================
  // Sample analog sensor
  // =====================
  if (t - lastSampleMs >= cfg.sampleMs) {
    lastSampleMs = t;

    int raw = analogRead(SENSOR_PIN);

    // EMA smoothing: filt = (1-a)*filt + a*raw
    float a = (float)cfg.emaAlphaPct / 100.0f;
    filt = (1.0f - a) * filt + a * (float)raw;

    // Apply threshold + hysteresis
    uint16_t thrHi = (cfg.threshold + cfg.hysteresis);
    uint16_t thrLo = (cfg.threshold > cfg.hysteresis) ? (cfg.threshold - cfg.hysteresis) : 0;

    bool newDetected = detected;

    // By default: detected becomes true when filt >= thrHi, and false when filt <= thrLo
    if (!detected && filt >= (float)thrHi) newDetected = true;
    else if (detected && filt <= (float)thrLo) newDetected = false;

    // invert logic if needed
    if (cfg.invert) newDetected = !newDetected;

    // Detect state change with debounce
    if (newDetected != detected) {
      if (t - lastChangeMs >= cfg.debounceMs) {
        bool prev = detected;
        detected = newDetected;
        lastChangeMs = t;

        Serial.print(F("[STATE] ")); Serial.print(prev ? F("ON") : F("OFF"));
        Serial.print(F(" -> "));     Serial.print(detected ? F("ON") : F("OFF"));
        Serial.print(F(" raw="));    Serial.print(raw);
        Serial.print(F(" filt="));   Serial.println((int)filt);

        // Count a lap on rising of "detected" (OFF->ON)
        if (!prev && detected) {
          unsigned long sinceLastLap = t - lastLapMs;

          if (sinceLastLap < cfg.holdoffMs) {
            Serial.print(F("[FILTER] holdoff: ")); Serial.print(sinceLastLap);
            Serial.print(F("ms < ")); Serial.print(cfg.holdoffMs);
            Serial.println(F("ms -> ignored"));
          } else if (cfg.minLapMs > 0 && sinceLastLap < cfg.minLapMs) {
            Serial.print(F("[FILTER] minlap: ")); Serial.print(sinceLastLap);
            Serial.print(F("ms < ")); Serial.print(cfg.minLapMs);
            Serial.println(F("ms -> ignored"));
          } else {
            lastLapMs = t;
            ble.print("LAP\n");
            digitalWrite(LED_PIN, !digitalRead(LED_PIN));

            Serial.print(F("[LAP] OK dt=")); Serial.print(sinceLastLap);
            Serial.print(F("ms -> SENT 'LAP'  raw=")); Serial.print(raw);
            Serial.print(F(" filt=")); Serial.println((int)filt);
          }
        }
      } else {
        Serial.println(F("[DEBOUNCE] State change ignored (too fast)"));
      }
    }

    // =====================
    // Heartbeat (every 500ms)
    // =====================
    if (t - lastPrintMs >= 500) {
      lastPrintMs = t;
      Serial.print(F("[HB] raw=")); Serial.print(raw);
      Serial.print(F(" filt=")); Serial.print((int)filt);
      Serial.print(F(" state=")); Serial.print(detected ? F("ON") : F("OFF"));
      Serial.print(F(" dtSinceLap=")); Serial.print(t - lastLapMs);
      Serial.print(F(" thr=")); Serial.print(cfg.threshold);
      Serial.print(F(" hyst=")); Serial.print(cfg.hysteresis);
      Serial.print(F(" inv=")); Serial.print(cfg.invert);
      Serial.print(F(" alpha=")); Serial.print(cfg.emaAlphaPct);
      Serial.print(F(" sampleMs=")); Serial.println(cfg.sampleMs);
    }
  }
}

