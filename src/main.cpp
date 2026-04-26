#include "config.h"

// Fallback if config.h is older and doesn't define TZ_STRING
#ifndef TZ_STRING
#define TZ_STRING "UTC0"
#endif

#include "esp32-hal.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "time.h"
#include <Adafruit_BME280.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_Sensor.h>
#include <Arduino.h>
#include <ESP32Time.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>

// Pin Definitions
#define ENCODER_CLK_PIN GPIO_NUM_4
#define ENCODER_DT_PIN GPIO_NUM_3
#define ENCODER_SW_PIN GPIO_NUM_2
#define MODE_BUTTON_PIN GPIO_NUM_5
#define I2C_SDA_PIN 6
#define I2C_SCL_PIN 7

// Display Settings
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

// Configuration
#define WIRE_SPEED 400000
#define NTP_STALENESS_INTERVAL                                                 \
  86400 // 1 hour = 3600, 6 hours = 21600, 1 day = 86400,
#define RAM_BUFFER_SIZE 48
#define MAX_FLASH_ENTRIES 8760
#define ENCODER_DEBOUNCE_MS 5 // Faster debounce, but ISR only on CLK
#define ENCODER_DETENTS_PER_CLICK                                              \
  2 // Not used in new ISR; keep for future tuning
#define CONFIRM_TIMEOUT_MS 10000 // Auto-cancel destructive prompts after 10 s

// Configurable sleep/wakeup options
const uint32_t SLEEP_OPTIONS_MS[] = {15000, 30000, 60000, 120000, 300000};
const char *SLEEP_LABELS[] = {"15s", "30s", "1m", "2m", "5m"};
const int SLEEP_OPTIONS_COUNT = 5;

const uint32_t WAKEUP_OPTIONS_MIN[] = {10, 15, 30, 60};
const char *WAKEUP_LABELS[] = {"10m", "15m", "30m", "1h"};
const int WAKEUP_OPTIONS_COUNT = 4;

// Settings menu (History is nested here now; Clear Data stays last)
enum SettingsItem {
  SET_NTP_SYNC,
  SET_MANUAL_TIME,
  SET_SLEEP,
  SET_WAKEUP,
  SET_HISTORY,
  SET_CLEAR_DATA,
  SETTINGS_COUNT
};

// Time-edit sub-fields
enum TimeEditField {
  TE_YEAR,
  TE_MONTH,
  TE_DAY,
  TE_HOUR,
  TE_MIN,
  TE_FIELD_COUNT
};

// Display modes (top-level, cycled by the dedicated mode button on GPIO 5)
enum DisplayMode {
  MODE_OVERVIEW,
  MODE_GRAPH,
  MODE_SETTINGS,
  MODE_COUNT
};

// Overview sub-pages (toggled by encoder rotation while in MODE_OVERVIEW)
enum OverviewPage {
  OV_DEFAULT,
  OV_CLOCK,
  OV_PAGE_COUNT
};

// Pending destructive/expensive action awaiting Yes/No confirmation
enum ConfirmAction {
  CONFIRM_NONE,
  CONFIRM_NTP_SYNC,
  CONFIRM_CLEAR_DATA
};

// Time ranges for graphs
enum TimeRange {
  RANGE_DAILY,   // 24h
  RANGE_WEEKLY,  // 7d
  RANGE_MONTHLY, // 30d
  RANGE_YEARLY   // 365d
};

// Data structure
struct SensorData {
  float temperature;
  float humidity;
  float pressure;
  time_t timestamp;
};

// RTC Memory
RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR bool backgroundReading = false;
RTC_DATA_ATTR uint32_t flashEntryCount = 0;
RTC_DATA_ATTR time_t oldestTimestamp = 0;
RTC_DATA_ATTR time_t newestTimestamp = 0;
RTC_DATA_ATTR time_t lastNtpSync = 0;

// Global objects
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_BME280 bme;
ESP32Time rtc(0); // UTC only
Preferences prefs;

// Hardware flags
bool displayAvailable = false;
bool sensorAvailable = false;

// Settings (persisted)
int sleepTimeoutIdx = 1;   // index into SLEEP_OPTIONS_MS, default 30s
int wakeupIntervalIdx = 2; // index into WAKEUP_OPTIONS_MIN, default 30min

// RAM buffer (24h at 30min intervals)
SensorData ramBuffer[RAM_BUFFER_SIZE];
int ramBufferCount = 0;

// Live update state (while awake)
SensorData liveData;
bool hasLiveData = false;
unsigned long lastLiveUpdate = 0;
unsigned long lastClockRedraw = 0;
unsigned long lastAnimTick = 0;
int animX = 0;
int animDir = 1;

// Encoder state (simplified)
volatile int encoderTicks = 0; // raw transitions from ISR
volatile uint8_t lastEncoderState = 0;
volatile bool buttonPressed = false;
volatile bool longPress = false;
volatile unsigned long buttonPressStart = 0;
volatile unsigned long lastEncoderTime = 0;
unsigned long lastActivityTime = 0;

// Mode button (separate from encoder click): cycles top mode + force sleep
volatile bool modeButtonPressed = false;
volatile bool modeLongPress = false;
volatile unsigned long modeButtonPressStart = 0;

// Filtered encoder state (in detent steps)
int encoderPos = 0; // logical position in steps
int lastProcessedTicks = 0;

// UI state
DisplayMode currentMode = MODE_OVERVIEW;
OverviewPage currentOverviewPage = OV_DEFAULT;
TimeRange currentRange = RANGE_DAILY;
int timeOffset = 0;
int historyIndex = 0;
bool graphShowTemp = true;
int settingsIndex = 0;
int settingsScroll = 0;

// Manual time-edit state
bool inTimeEditMode = false;
int timeEditField = TE_YEAR;
struct tm timeEditBuf = {};

// History sub-view (nested under Settings -> History)
bool inHistoryView = false;

// Confirmation prompt state (NTP sync / Clear data)
ConfirmAction pendingConfirm = CONFIRM_NONE;
unsigned long confirmEnterMs = 0;
unsigned long lastConfirmRedraw = 0;

// Clock overview slide-on-minute-change state
int prevMinute = -1;
unsigned long minuteSlideStart = 0;
bool minuteSliding = false;

// ========== ISRs ==========

void IRAM_ATTR encoderISR() {
  unsigned long now = millis();

  // Simple debounce on the encoder
  if (now - lastEncoderTime < ENCODER_DEBOUNCE_MS)
    return;

  uint8_t clk = digitalRead(ENCODER_CLK_PIN);
  uint8_t dt = digitalRead(ENCODER_DT_PIN);
  uint8_t state = (clk << 1) | dt;

  // Only act on a CLK falling edge for stability:
  // previous CLK bit = 1, current clk = 0
  if ((lastEncoderState & 0b10) && !clk) {
    if (dt) {
      // dt = 1 on clk falling => one direction
      encoderTicks++;
    } else {
      // dt = 0 on clk falling => other direction
      encoderTicks--;
    }

    lastEncoderTime = now;
    lastActivityTime = now;
  }

  lastEncoderState = state;
}

void IRAM_ATTR buttonISR() {
  unsigned long now = millis();

  if (digitalRead(ENCODER_SW_PIN) == LOW) {
    buttonPressStart = now;
  } else {
    unsigned long pressDuration = now - buttonPressStart;
    if (pressDuration > 50 && pressDuration < 500) {
      buttonPressed = true;
    } else if (pressDuration >= 500) {
      longPress = true;
    }
  }
  lastActivityTime = now;
}

void IRAM_ATTR modeButtonISR() {
  unsigned long now = millis();

  if (digitalRead(MODE_BUTTON_PIN) == LOW) {
    modeButtonPressStart = now;
  } else {
    unsigned long pressDuration = now - modeButtonPressStart;
    if (pressDuration > 50 && pressDuration < 500) {
      modeButtonPressed = true;
    } else if (pressDuration >= 500) {
      modeLongPress = true;
    }
  }
  lastActivityTime = now;
}

// ========== Hardware Initialization ==========

void setupWakeupSources() {
  esp_deep_sleep_enable_gpio_wakeup((1ULL << ENCODER_SW_PIN) |
                                        (1ULL << ENCODER_CLK_PIN) |
                                        (1ULL << ENCODER_DT_PIN) |
                                        (1ULL << MODE_BUTTON_PIN),
                                    ESP_GPIO_WAKEUP_GPIO_LOW);
}

void setupEncoder() {
  pinMode(ENCODER_SW_PIN, INPUT_PULLUP);
  pinMode(ENCODER_CLK_PIN, INPUT_PULLUP);
  pinMode(ENCODER_DT_PIN, INPUT_PULLUP);
  pinMode(MODE_BUTTON_PIN, INPUT_PULLUP);

  // Initial state
  lastEncoderState =
      (digitalRead(ENCODER_CLK_PIN) << 1) | digitalRead(ENCODER_DT_PIN);
  encoderTicks = 0;
  encoderPos = 0;
  lastProcessedTicks = 0;

  // Only use CLK pin interrupt for rotation, FALLING edge
  attachInterrupt(digitalPinToInterrupt(ENCODER_CLK_PIN), encoderISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(ENCODER_SW_PIN), buttonISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(MODE_BUTTON_PIN), modeButtonISR,
                  CHANGE);
}

// ========== Time & Settings Helpers ==========

void loadSettings() {
  prefs.begin("settings", true);
  lastNtpSync = prefs.getULong("lastNtpSync", 0);
  sleepTimeoutIdx = prefs.getInt("sleepIdx", 1);
  wakeupIntervalIdx = prefs.getInt("wakeupIdx", 2);
  prefs.end();

  // Clamp indices to valid range
  if (sleepTimeoutIdx < 0 || sleepTimeoutIdx >= SLEEP_OPTIONS_COUNT)
    sleepTimeoutIdx = 1;
  if (wakeupIntervalIdx < 0 || wakeupIntervalIdx >= WAKEUP_OPTIONS_COUNT)
    wakeupIntervalIdx = 2;

  Serial.printf("Settings loaded: TZ=%s, Sleep=%s, Wakeup=%s\n", TZ_STRING,
                SLEEP_LABELS[sleepTimeoutIdx],
                WAKEUP_LABELS[wakeupIntervalIdx]);
}

void saveSettings() {
  prefs.begin("settings", false);
  prefs.putULong("lastNtpSync", lastNtpSync);
  prefs.putInt("sleepIdx", sleepTimeoutIdx);
  prefs.putInt("wakeupIdx", wakeupIntervalIdx);
  prefs.end();
  Serial.printf("Settings saved: Sleep=%s, Wakeup=%s\n",
                SLEEP_LABELS[sleepTimeoutIdx],
                WAKEUP_LABELS[wakeupIntervalIdx]);
}

void initDisplay() {
  Serial.println("Initializing display...");
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("SSD1306 not found");
    displayAvailable = false;
    return;
  }
  displayAvailable = true;
  Serial.println("Display OK");
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.display();
}

void printWiFiStatus() {
  Serial.println("\n=== WiFi Debug ===");
  Serial.printf("SSID Length: %d\n", strlen(WIFI_SSID));
  Serial.printf("SSID: %s\n", WIFI_SSID);
  Serial.printf("WiFi Status: %d\n", WiFi.status());
  Serial.printf("WiFi Mode: %d\n", WiFi.getMode());
  Serial.println("==================\n");
}

void logScanResults() {
  Serial.println("Scanning 2.4 GHz networks...");
  int n = WiFi.scanNetworks(false, true); // include hidden
  if (n <= 0) {
    Serial.println("  (no networks found)");
    return;
  }
  Serial.printf("  Found %d networks:\n", n);
  for (int i = 0; i < n && i < 15; i++) {
    Serial.printf("  %2d: %-32s ch%2d %4d dBm  %s\n", i + 1,
                  WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i),
                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "OPEN" : "secured");
  }
  WiFi.scanDelete();
}

// Map ESP-IDF wifi disconnect reason codes to short strings
static const char *wifiDisconnectReason(uint8_t reason) {
  switch (reason) {
  case 1:
    return "UNSPECIFIED";
  case 2:
    return "AUTH_EXPIRE";
  case 3:
    return "AUTH_LEAVE";
  case 4:
    return "ASSOC_EXPIRE";
  case 5:
    return "ASSOC_TOOMANY";
  case 6:
    return "NOT_AUTHED";
  case 7:
    return "NOT_ASSOCED";
  case 8:
    return "ASSOC_LEAVE";
  case 9:
    return "ASSOC_NOT_AUTHED";
  case 13:
    return "INVALID_IE";
  case 14:
    return "MIC_FAILURE";
  case 15:
    return "4WAY_HANDSHAKE_TIMEOUT (likely WRONG PASSWORD)";
  case 16:
    return "GROUP_KEY_UPDATE_TIMEOUT";
  case 17:
    return "IE_IN_4WAY_DIFFERS (encryption mismatch)";
  case 18:
    return "GROUP_CIPHER_INVALID";
  case 19:
    return "PAIRWISE_CIPHER_INVALID";
  case 20:
    return "AKMP_INVALID";
  case 23:
    return "IEEE_802_1X_AUTH_FAILED";
  case 24:
    return "CIPHER_SUITE_REJECTED";
  case 200:
    return "BEACON_TIMEOUT";
  case 201:
    return "NO_AP_FOUND";
  case 202:
    return "AUTH_FAIL";
  case 203:
    return "ASSOC_FAIL";
  case 204:
    return "HANDSHAKE_TIMEOUT (likely WRONG PASSWORD)";
  case 205:
    return "CONNECTION_FAIL";
  case 206:
    return "AP_TSF_RESET";
  case 207:
    return "ROAMING";
  default:
    return "UNKNOWN";
  }
}

bool syncTimeWithNTP() {
  // ---- 1. Bring the radio up cleanly ----
  // Order matters on ESP32-C3: driver must be STARTED before esp_wifi_set_*.
  // WiFi.disconnect(true, ...) turns the radio OFF, which makes subsequent
  // esp_wifi_* calls silently fail.
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  delay(50);
  WiFi.mode(WIFI_STA); // starts the driver
  delay(100);

  // Log disconnect reasons (helps diagnose auth/assoc failures)
  WiFi.onEvent(
      [](WiFiEvent_t event, WiFiEventInfo_t info) {
        uint8_t reason = info.wifi_sta_disconnected.reason;
        Serial.printf("[WiFi] Disconnect reason %u: %s\n", reason,
                      wifiDisconnectReason(reason));
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  // ---- 2. Apply C3-friendly settings (now that driver is up) ----
  // - Set EU country code so ch 12-13 are also scanned (default may be ch1-11).
  // - Some APs reject Long-Range mode beacons. Force plain B/G/N.
  // - C3 only supports HT20; avoid HT40 negotiation.
  // - Max TX power for marginal links.
  wifi_country_t country = {
      .cc = "DE",
      .schan = 1,
      .nchan = 13,
      .max_tx_power = 84, // 0.25 dBm units => 21 dBm
      .policy = WIFI_COUNTRY_POLICY_MANUAL,
  };
  esp_wifi_set_country(&country);
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                                         WIFI_PROTOCOL_11N);
  esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  // ---- 3. Scan first to locate the AP on a specific channel/BSSID ----
  // Use a longer dwell time (500ms/ch) so faint beacons are caught.
  // This avoids AiMesh band-steering and tells us if the SSID is visible.
  Serial.printf("Scanning for '%s'...\n", WIFI_SSID);
  int n = WiFi.scanNetworks(false, true, false, 500); // sync, hidden, active, 500ms/ch
  int targetIdx = -1;
  int32_t bestRssi = -127;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == WIFI_SSID && WiFi.RSSI(i) > bestRssi) {
      bestRssi = WiFi.RSSI(i);
      targetIdx = i;
    }
  }

  // Log nearby networks either way - useful diagnostic
  Serial.printf("Found %d networks:\n", n);
  for (int i = 0; i < n && i < 15; i++) {
    Serial.printf("  %2d: %-32s ch%2d %4d dBm\n", i + 1,
                  WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i));
  }

  // ---- 4. Diagnostic: dump SSID + PSK byte-for-byte ----
  // Detects accidental non-ASCII whitespace, BOM, trailing CR, etc.
  Serial.printf("SSID len=%u: ", (unsigned)strlen(WIFI_SSID));
  for (size_t i = 0; i < strlen(WIFI_SSID); i++)
    Serial.printf("%02X ", (uint8_t)WIFI_SSID[i]);
  Serial.println();
  Serial.printf("PSK  len=%u: ", (unsigned)strlen(WIFI_PASSWORD));
  for (size_t i = 0; i < strlen(WIFI_PASSWORD); i++)
    Serial.printf("%02X ", (uint8_t)WIFI_PASSWORD[i]);
  Serial.println();

  // ---- 5. Connect ----
  if (targetIdx >= 0) {
    uint8_t *bssid = WiFi.BSSID(targetIdx);
    int ch = WiFi.channel(targetIdx);
    Serial.printf("Target on ch%d %d dBm %s - pinning channel+BSSID\n", ch,
                  WiFi.RSSI(targetIdx), WiFi.BSSIDstr(targetIdx).c_str());
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD, ch, bssid);
  } else {
    Serial.printf("'%s' NOT in scan results - trying blind connect\n",
                  WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
  WiFi.scanDelete();

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    if (WiFi.status() == WL_CONNECT_FAILED && attempts < 5) {
      Serial.println("\nConnect failed. Bouncing...");
      WiFi.disconnect(false); // don't turn radio off
      delay(500);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    attempts++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("\nFailed. Final Status: %d\n", WiFi.status());
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return false;
  }

  Serial.printf("\nConnected. IP=%s RSSI=%d dBm CH=%d\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                WiFi.channel());

  // ---- 5. NTP ----
  Serial.println("Syncing NTP...");
  configTzTime(TZ_STRING, NTP_SERVER);

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10000)) {
    Serial.println("NTP Sync Failed");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return false;
  }

  rtc.setTimeStruct(timeinfo);
  lastNtpSync = rtc.getEpoch();
  saveSettings();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  return true;
}

bool shouldSyncNtp() {
  time_t now = rtc.getEpoch();

  if (backgroundReading) {
    return true;
  }

  // Never synced
  if (lastNtpSync == 0 || (now - lastNtpSync > NTP_STALENESS_INTERVAL)) {
    Serial.println("NTP: Never synced or is stale, attempting...");
    return true;
  }

  Serial.printf("NTP: Last sync %ld seconds ago, skipping\n",
                now - lastNtpSync);
  return false;
}

bool timeIsSane() { return rtc.getEpoch() >= 1767225600; }

// ========== Manual Time-Edit Helpers ==========

static int daysInMonth(int year1900, int mon0) {
  static const int days[] = {31, 28, 31, 30, 31, 30,
                             31, 31, 30, 31, 30, 31};
  if (mon0 == 1) {
    int y = year1900 + 1900;
    bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    return leap ? 29 : 28;
  }
  return days[mon0];
}

void enterTimeEditMode() {
  time_t utc = rtc.getEpoch();
  localtime_r(&utc, &timeEditBuf); // TZ env (set in setup) gives local time
  timeEditBuf.tm_sec = 0;
  // Clamp year to a sane range so the user starts somewhere reasonable
  if (timeEditBuf.tm_year < 125)
    timeEditBuf.tm_year = 125; // 2025
  if (timeEditBuf.tm_year > 199)
    timeEditBuf.tm_year = 199; // 2099
  timeEditField = TE_YEAR;
  inTimeEditMode = true;
  Serial.println("Manual time edit: ENTER");
}

void adjustTimeEditField(int delta) {
  switch (timeEditField) {
  case TE_YEAR: {
    int y = timeEditBuf.tm_year + delta;
    if (y < 125)
      y = 125; // 2025
    if (y > 199)
      y = 199; // 2099
    timeEditBuf.tm_year = y;
    int dim = daysInMonth(timeEditBuf.tm_year, timeEditBuf.tm_mon);
    if (timeEditBuf.tm_mday > dim)
      timeEditBuf.tm_mday = dim;
    break;
  }
  case TE_MONTH: {
    int m = timeEditBuf.tm_mon + delta;
    while (m < 0)
      m += 12;
    m %= 12;
    timeEditBuf.tm_mon = m;
    int dim = daysInMonth(timeEditBuf.tm_year, m);
    if (timeEditBuf.tm_mday > dim)
      timeEditBuf.tm_mday = dim;
    break;
  }
  case TE_DAY: {
    int dim = daysInMonth(timeEditBuf.tm_year, timeEditBuf.tm_mon);
    int d = timeEditBuf.tm_mday + delta;
    while (d < 1)
      d += dim;
    d = ((d - 1) % dim) + 1;
    timeEditBuf.tm_mday = d;
    break;
  }
  case TE_HOUR: {
    int h = timeEditBuf.tm_hour + delta;
    while (h < 0)
      h += 24;
    h %= 24;
    timeEditBuf.tm_hour = h;
    break;
  }
  case TE_MIN: {
    int mi = timeEditBuf.tm_min + delta;
    while (mi < 0)
      mi += 60;
    mi %= 60;
    timeEditBuf.tm_min = mi;
    break;
  }
  default:
    break;
  }
}

void saveTimeEdit() {
  // The buffer holds the user-entered LOCAL time. mktime() interprets it
  // according to the TZ env var (set in setup) and returns the UTC epoch,
  // automatically applying DST rules from the POSIX TZ string.
  timeEditBuf.tm_isdst = -1; // let mktime auto-detect DST for this date
  time_t newUtc = mktime(&timeEditBuf);
  rtc.setTime(newUtc);
  // Mark as recently-set so we don't immediately overwrite via NTP next boot
  lastNtpSync = newUtc;
  saveSettings();
  Serial.printf("Manual time set: UTC epoch = %ld\n", (long)newUtc);
}

void initSensor() {
  Serial.println("Initializing BME280...");
  if (!bme.begin(0x76, &Wire)) {
    Serial.println("BME280 not found");
    sensorAvailable = false;
    return;
  }
  sensorAvailable = true;
  Serial.println("BME280 OK");
}

// ========== Flash Persistence ==========

void loadRamBuffer() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    return;
  }

  File file = LittleFS.open("/history.dat", FILE_READ);
  if (!file) {
    Serial.println("No history file");
    flashEntryCount = 0;
    ramBufferCount = 0;
    return;
  }

  flashEntryCount = file.size() / sizeof(SensorData);
  ramBufferCount = min((int)flashEntryCount, RAM_BUFFER_SIZE);

  if (ramBufferCount > 0) {
    file.seek((flashEntryCount - ramBufferCount) * sizeof(SensorData));
    for (int i = 0; i < ramBufferCount; i++) {
      file.read((uint8_t *)&ramBuffer[i], sizeof(SensorData));
    }
    newestTimestamp = ramBuffer[ramBufferCount - 1].timestamp;
    file.seek(0);
    SensorData first;
    file.read((uint8_t *)&first, sizeof(SensorData));
    oldestTimestamp = first.timestamp;
  }
  file.close();

  Serial.printf("Loaded %d entries (total: %lu)\n", ramBufferCount,
                flashEntryCount);
}

void logReading(const SensorData &data) {
  // Add to RAM buffer
  if (ramBufferCount < RAM_BUFFER_SIZE) {
    ramBuffer[ramBufferCount++] = data;
  } else {
    for (int i = 0; i < RAM_BUFFER_SIZE - 1; i++) {
      ramBuffer[i] = ramBuffer[i + 1];
    }
    ramBuffer[RAM_BUFFER_SIZE - 1] = data;
  }

  // Append to flash file
  if (!LittleFS.begin(true))
    return;

  File file = LittleFS.open("/history.dat", FILE_APPEND);
  if (file) {
    file.write((const uint8_t *)&data, sizeof(SensorData));
    file.close();
    flashEntryCount++;

    newestTimestamp = data.timestamp;
    if (flashEntryCount == 1)
      oldestTimestamp = data.timestamp;

    Serial.printf("Logged #%lu\n", flashEntryCount);
  } else {
    Serial.println("Failed to open history file");
  }
}

SensorData getHistoryEntry(int index) {
  // Check RAM buffer first
  if (index >= flashEntryCount - ramBufferCount && index < flashEntryCount) {
    int ramIdx = ramBufferCount - (flashEntryCount - index);
    return ramBuffer[ramIdx];
  }

  // Load from file
  SensorData data = {0};
  if (!LittleFS.begin())
    return data;

  File file = LittleFS.open("/history.dat", FILE_READ);
  if (file) {
    file.seek(index * sizeof(SensorData));
    file.read((uint8_t *)&data, sizeof(SensorData));
    file.close();
  }
  return data;
}

void readAndLogSensor() {
  if (!sensorAvailable) {
    Serial.println("No sensor");
    return;
  }

  SensorData data;
  data.temperature = bme.readTemperature();
  data.humidity = bme.readHumidity();
  data.pressure = bme.readPressure() / 100.0F;
  data.timestamp = rtc.getEpoch();

  logReading(data);

  Serial.println("=== Reading ===");
  Serial.printf("T: %.1f C  H: %.0f%%  P: %.0fhPa\n", data.temperature,
                data.humidity, data.pressure);
  Serial.printf("Time: %s\n", rtc.getTime("%H:%M:%S").c_str());
  Serial.printf("Entries: %lu (RAM: %d)\n", flashEntryCount, ramBufferCount);
  Serial.println("===============");
}

bool readSensorLive(SensorData &out) {
  if (!sensorAvailable)
    return false;
  out.temperature = bme.readTemperature();
  out.humidity = bme.readHumidity();
  out.pressure = bme.readPressure() / 100.0F;
  out.timestamp = rtc.getEpoch();
  return true;
}

void clearHistory() {
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed (clear)");
    return;
  }
  if (LittleFS.exists("/history.dat")) {
    LittleFS.remove("/history.dat");
  }
  flashEntryCount = 0;
  ramBufferCount = 0;
  oldestTimestamp = 0;
  newestTimestamp = 0;
  Serial.println("History cleared");
}

// ========== Display Functions ==========

void displayOverviewDefault() {
  if (!displayAvailable || ramBufferCount == 0)
    return;

  SensorData &data = hasLiveData ? liveData : ramBuffer[ramBufferCount - 1];

  display.clearDisplay();

  // Convert UTC to local time (TZ env handles DST automatically)
  time_t utc = rtc.getEpoch();
  struct tm timeinfo;
  localtime_r(&utc, &timeinfo);

  // Top: Date and Time (local)
  display.setTextSize(1);
  display.setCursor(0, 0);
  char dateStr[16];
  strftime(dateStr, sizeof(dateStr), "%a %d.%m.%y", &timeinfo);
  display.print(dateStr);

  display.setCursor(85, 0);
  char timeStr[6];
  strftime(timeStr, sizeof(timeStr), "%H:%M", &timeinfo);
  display.print(timeStr);
  if (millis() - lastAnimTick > 150) {
    animX += animDir;
    if (animX < 0) {
      animX = 0;
      animDir = 1;
    }
    if (animX > 20) {
      animX = 20;
      animDir = -1;
    }
    lastAnimTick = millis();
  }
  display.drawPixel(60 + animX, 10, SSD1306_WHITE);

  // Center: Temperature (large)
  display.setTextSize(3);
  display.setCursor(0, 18);
  char tempStr[8];
  snprintf(tempStr, sizeof(tempStr), "%.1f", data.temperature);
  display.print(tempStr);
  display.print(" ");
  display.write(247);
  display.print("C");

  // Bottom: Humidity and Pressure
  display.setTextSize(1);
  display.setCursor(0, 48);
  display.print("H:");
  display.print(data.humidity, 0);
  display.print("%");

  display.setCursor(70, 48);
  display.print("P:");
  display.print(data.pressure, 0);

  display.display();
}

// Helper: draw a single size-4 character (24 wide x 32 tall cell) at (x, y).
static void drawClockChar(int x, int y, char c) {
  display.setTextSize(4);
  display.setCursor(x, y);
  display.write(c);
}

// Big clock with minute slide-flip animation, date on top, T/H on bottom.
void displayOverviewClock() {
  if (!displayAvailable)
    return;

  SensorData fallback = {0};
  SensorData &data = hasLiveData
                         ? liveData
                         : (ramBufferCount > 0 ? ramBuffer[ramBufferCount - 1]
                                               : fallback);

  time_t utc = rtc.getEpoch();
  struct tm timeinfo;
  localtime_r(&utc, &timeinfo);

  // Detect minute change and arm the slide animation.
  if (prevMinute < 0) {
    prevMinute = timeinfo.tm_min;
  } else if (timeinfo.tm_min != prevMinute && !minuteSliding) {
    minuteSliding = true;
    minuteSlideStart = millis();
  }

  // Compute slide progress (0..1). When done, latch new minute.
  const unsigned long SLIDE_MS = 250;
  float progress = 0.0f;
  bool sliding = false;
  if (minuteSliding) {
    unsigned long elapsed = millis() - minuteSlideStart;
    if (elapsed >= SLIDE_MS) {
      minuteSliding = false;
      prevMinute = timeinfo.tm_min;
    } else {
      progress = (float)elapsed / (float)SLIDE_MS;
      sliding = true;
    }
  }

  display.clearDisplay();

  // ---- Big HH:MM band ----
  // 5 cells of 24 px = 120 px wide, x_start = 4. Band y = 16..47 (32 tall).
  const int CLOCK_Y = 16;
  const int DIGIT_W = 24;
  const int CLOCK_X = 4;
  const int DIGIT_H = 32;

  int hh = timeinfo.tm_hour;
  int curMin = timeinfo.tm_min;
  int oldMin = (prevMinute >= 0) ? prevMinute : curMin;

  // Hours (static)
  drawClockChar(CLOCK_X + 0 * DIGIT_W, CLOCK_Y, '0' + (hh / 10));
  drawClockChar(CLOCK_X + 1 * DIGIT_W, CLOCK_Y, '0' + (hh % 10));

  // Colon (blink at 1 Hz; always on while sliding for steadier reading)
  bool colonOn = sliding || ((millis() / 500) % 2 == 0);
  if (colonOn) {
    drawClockChar(CLOCK_X + 2 * DIGIT_W, CLOCK_Y, ':');
  }

  // Minutes
  if (sliding) {
    int oldOff = -(int)(progress * DIGIT_H); // old slides up off the band
    int newOff = (int)((1.0f - progress) * DIGIT_H); // new slides up into band
    drawClockChar(CLOCK_X + 3 * DIGIT_W, CLOCK_Y + oldOff, '0' + (oldMin / 10));
    drawClockChar(CLOCK_X + 4 * DIGIT_W, CLOCK_Y + oldOff, '0' + (oldMin % 10));
    drawClockChar(CLOCK_X + 3 * DIGIT_W, CLOCK_Y + newOff, '0' + (curMin / 10));
    drawClockChar(CLOCK_X + 4 * DIGIT_W, CLOCK_Y + newOff, '0' + (curMin % 10));
  } else {
    drawClockChar(CLOCK_X + 3 * DIGIT_W, CLOCK_Y, '0' + (curMin / 10));
    drawClockChar(CLOCK_X + 4 * DIGIT_W, CLOCK_Y, '0' + (curMin % 10));
  }

  // Mask digit overflow above and below the clock band so the date row and
  // T/H row stay clean during the slide animation.
  display.fillRect(0, 0, SCREEN_WIDTH, CLOCK_Y, SSD1306_BLACK);
  display.fillRect(0, CLOCK_Y + DIGIT_H, SCREEN_WIDTH,
                   SCREEN_HEIGHT - (CLOCK_Y + DIGIT_H), SSD1306_BLACK);

  // ---- Date row (top) ----
  display.setTextSize(1);
  char dateStr[20];
  strftime(dateStr, sizeof(dateStr), "%a %d.%m.%y", &timeinfo);
  int dateW = (int)strlen(dateStr) * 6;
  int dateX = (SCREEN_WIDTH - dateW) / 2;
  if (dateX < 0)
    dateX = 0;
  display.setCursor(dateX, 4);
  display.print(dateStr);

  // ---- T/H row (bottom) ----
  char thStr[24];
  if (hasLiveData || ramBufferCount > 0) {
    snprintf(thStr, sizeof(thStr), "T:%.1fC  H:%.0f%%", data.temperature,
             data.humidity);
  } else {
    snprintf(thStr, sizeof(thStr), "no data");
  }
  int thW = (int)strlen(thStr) * 6;
  int thX = (SCREEN_WIDTH - thW) / 2;
  if (thX < 0)
    thX = 0;
  display.setCursor(thX, 56);
  display.print(thStr);

  display.display();
}

// Dispatcher: pick the active overview sub-page.
void displayOverview() {
  if (currentOverviewPage == OV_CLOCK) {
    displayOverviewClock();
  } else {
    displayOverviewDefault();
  }
}

void displayHistory() {
  if (!displayAvailable)
    return;

  int totalEntries = flashEntryCount;
  if (totalEntries == 0) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(20, 28);
    display.print("No history");
    display.display();
    return;
  }

  if (historyIndex < 0)
    historyIndex = 0;
  if (historyIndex >= totalEntries)
    historyIndex = totalEntries - 1;

  SensorData data = getHistoryEntry(historyIndex);

  display.clearDisplay();
  display.setTextSize(1);

  // Header (annotate as nested under Settings -> History)
  display.setCursor(0, 0);
  display.printf("[#%d/%d] History", historyIndex + 1, totalEntries);

  // Timestamp (convert UTC to local; TZ env handles DST)
  time_t utc = data.timestamp;
  struct tm timeinfo;
  localtime_r(&utc, &timeinfo);
  char timeStr[20];
  strftime(timeStr, sizeof(timeStr), "%H:%M %d.%m.%y", &timeinfo);

  display.setCursor(0, 12);
  display.print(timeStr);

  // Values
  display.setCursor(0, 28);
  display.print("T: ");
  display.print(data.temperature, 1);
  display.print("C");

  display.setCursor(0, 40);
  display.print("H: ");
  display.print(data.humidity, 0);
  display.print("%");

  display.setCursor(0, 52);
  display.print("P: ");
  display.print(data.pressure, 0);
  display.print("hPa");

  display.display();
}

void displaySettings() {
  if (!displayAvailable)
    return;

  display.clearDisplay();
  display.setTextSize(1);

  // Header
  display.setCursor(0, 0);
  display.print("SETTINGS");

  // Up to 5 visible items, 9px spacing starting at y=11
  const int VISIBLE_ITEMS = 5;
  const int itemY[VISIBLE_ITEMS] = {11, 20, 29, 38, 47};

  // Adjust scroll window so the selected item is visible
  if (settingsIndex < settingsScroll)
    settingsScroll = settingsIndex;
  if (settingsIndex >= settingsScroll + VISIBLE_ITEMS)
    settingsScroll = settingsIndex - VISIBLE_ITEMS + 1;
  if (settingsScroll < 0)
    settingsScroll = 0;
  if (settingsScroll > SETTINGS_COUNT - VISIBLE_ITEMS)
    settingsScroll = max(0, SETTINGS_COUNT - VISIBLE_ITEMS);

  for (int slot = 0; slot < VISIBLE_ITEMS; slot++) {
    int i = settingsScroll + slot;
    if (i >= SETTINGS_COUNT)
      break;

    display.setCursor(0, itemY[slot]);
    display.print(i == settingsIndex ? ">" : " ");

    switch (i) {
    case SET_NTP_SYNC:
      display.print(" NTP Sync");
      break;
    case SET_MANUAL_TIME:
      display.print(" Set Time");
      break;
    case SET_SLEEP:
      display.printf(" Sleep: %s", SLEEP_LABELS[sleepTimeoutIdx]);
      break;
    case SET_WAKEUP:
      display.printf(" Wakeup: %s", WAKEUP_LABELS[wakeupIntervalIdx]);
      break;
    case SET_HISTORY:
      display.print(" History");
      break;
    case SET_CLEAR_DATA:
      display.print(" Clear Data");
      break;
    }
  }

  // Scroll indicators
  if (settingsScroll > 0) {
    display.setCursor(122, 11);
    display.print((char)0x18); // up arrow
  }
  if (settingsScroll + VISIBLE_ITEMS < SETTINGS_COUNT) {
    display.setCursor(122, 47);
    display.print((char)0x19); // down arrow
  }

  // Footer hint (settings auto-save; no Hold-to-save anymore)
  display.setCursor(0, 57);
  display.print("Turn=Nav  Click=Pick");

  display.display();
}

// Yes/No confirmation prompt for destructive/expensive actions.
// Encoder click = Yes, mode button = No, auto-cancel after CONFIRM_TIMEOUT_MS.
void displayConfirm() {
  if (!displayAvailable)
    return;

  display.clearDisplay();
  display.setTextSize(1);

  // Header centered
  const char *title = "CONFIRM";
  int titleW = (int)strlen(title) * 6;
  display.setCursor((SCREEN_WIDTH - titleW) / 2, 0);
  display.print(title);

  // Question (two lines)
  const char *line1 = "";
  const char *line2 = "";
  switch (pendingConfirm) {
  case CONFIRM_CLEAR_DATA:
    line1 = "Clear all logged";
    line2 = "data? Cannot undo.";
    break;
  case CONFIRM_NTP_SYNC:
    line1 = "Sync time via WiFi?";
    line2 = "Takes ~10s.";
    break;
  default:
    line1 = "Are you sure?";
    line2 = "";
    break;
  }
  int l1W = (int)strlen(line1) * 6;
  int l2W = (int)strlen(line2) * 6;
  display.setCursor((SCREEN_WIDTH - l1W) / 2, 18);
  display.print(line1);
  display.setCursor((SCREEN_WIDTH - l2W) / 2, 30);
  display.print(line2);

  // Hint row
  const char *hint = "Yes=Click  No=Mode";
  int hintW = (int)strlen(hint) * 6;
  display.setCursor((SCREEN_WIDTH - hintW) / 2, 44);
  display.print(hint);

  // Countdown bar + remaining seconds
  unsigned long elapsed = millis() - confirmEnterMs;
  if (elapsed > CONFIRM_TIMEOUT_MS)
    elapsed = CONFIRM_TIMEOUT_MS;
  unsigned long remaining = CONFIRM_TIMEOUT_MS - elapsed;
  int barX = 4;
  int barY = 56;
  int barH = 6;
  int barMaxW = 100;
  int barW = (int)((long)remaining * barMaxW / CONFIRM_TIMEOUT_MS);
  // Border around the bar's full width
  display.drawRect(barX - 1, barY - 1, barMaxW + 2, barH + 2, SSD1306_WHITE);
  if (barW > 0) {
    display.fillRect(barX, barY, barW, barH, SSD1306_WHITE);
  }
  // Seconds remaining (rounded up, so it shows "10s" -> "0s")
  int secs = (int)((remaining + 999) / 1000);
  char secBuf[6];
  snprintf(secBuf, sizeof(secBuf), "%ds", secs);
  display.setCursor(SCREEN_WIDTH - (int)strlen(secBuf) * 6 - 2, 56);
  display.print(secBuf);

  display.display();
}

void displayTimeEdit() {
  if (!displayAvailable)
    return;

  display.clearDisplay();
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.print("SET LOCAL TIME");

  // Format: YYYY-MM-DD HH:MM (16 chars). At size-1 (6px/char) = 96px wide.
  // Start at x=4 so we have a comfortable margin.
  char buf[24];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
           timeEditBuf.tm_year + 1900, timeEditBuf.tm_mon + 1,
           timeEditBuf.tm_mday, timeEditBuf.tm_hour, timeEditBuf.tm_min);
  const int textY = 18;
  const int textX = 4;
  display.setCursor(textX, textY);
  display.print(buf);

  // Underline the selected field. Char positions in buf:
  //   year   = 0..3   (4 chars)
  //   month  = 5..6   (2 chars)
  //   day    = 8..9   (2 chars)
  //   hour   = 11..12 (2 chars)
  //   minute = 14..15 (2 chars)
  const int charW = 6;
  const int fieldStartCol[TE_FIELD_COUNT] = {0, 5, 8, 11, 14};
  const int fieldLenChars[TE_FIELD_COUNT] = {4, 2, 2, 2, 2};
  int ux = textX + fieldStartCol[timeEditField] * charW;
  int uw = fieldLenChars[timeEditField] * charW;
  display.drawFastHLine(ux, textY + 9, uw, SSD1306_WHITE);

  // Hints
  display.setCursor(0, 38);
  display.print("Turn: change");
  display.setCursor(0, 47);
  display.print("Press: next field");
  display.setCursor(0, 56);
  display.print("Hold: save & exit");

  display.display();
}

// Get data series for graph based on range and offset (OPTIMIZED)
void getSeries(float *values, int *count, bool isTemperature, TimeRange range,
               int offset) {
  time_t now = rtc.getEpoch();
  time_t rangeSeconds;

  switch (range) {
  case RANGE_DAILY:
    rangeSeconds = 86400;
    break; // 24h
  case RANGE_WEEKLY:
    rangeSeconds = 604800;
    break; // 7d
  case RANGE_MONTHLY:
    rangeSeconds = 2592000;
    break; // 30d
  case RANGE_YEARLY:
    rangeSeconds = 31536000;
    break; // 365d
  }

  time_t startTime = now - rangeSeconds * (offset + 1);
  time_t endTime = now - rangeSeconds * offset;

  *count = 0;

  // Early exit if no data in range
  if (flashEntryCount == 0 || endTime < oldestTimestamp ||
      startTime > newestTimestamp) {
    return;
  }

  // Estimate entry range using timestamps (assuming roughly even spacing)
  int estimatedStart = 0;
  int estimatedEnd = flashEntryCount;

  if (flashEntryCount > 0 && newestTimestamp > oldestTimestamp) {
    float entriesPerSecond =
        (float)flashEntryCount / (newestTimestamp - oldestTimestamp);
    estimatedStart =
        max(0, (int)((startTime - oldestTimestamp) * entriesPerSecond) - 10);
    estimatedEnd =
        min((int)flashEntryCount,
            (int)((endTime - oldestTimestamp) * entriesPerSecond) + 10);
  }

  // Scan only estimated range
  int matchingCount = 0;
  for (int i = estimatedStart; i < estimatedEnd; i++) {
    SensorData data = getHistoryEntry(i);
    if (data.timestamp >= startTime && data.timestamp <= endTime) {
      matchingCount++;
    }
  }

  if (matchingCount == 0)
    return;

  // Calculate downsample step
  int maxPoints = 120;
  int step = max(1, matchingCount / maxPoints);

  // Collect downsampled data
  int collected = 0;
  for (int i = estimatedStart; i < estimatedEnd && *count < maxPoints; i++) {
    if (collected % step != 0) {
      collected++;
      continue;
    }

    SensorData data = getHistoryEntry(i);
    if (data.timestamp >= startTime && data.timestamp <= endTime) {
      values[*count] = isTemperature ? data.temperature : data.humidity;
      (*count)++;
      collected++;
    }
  }
}

void drawGraph(bool isTemperature) {
  if (!displayAvailable)
    return;

  display.clearDisplay();
  display.setTextSize(1);

  // Title with range info
  display.setCursor(0, 0);
  if (isTemperature) {
    display.print("Temp ");
  } else {
    display.print("Humid ");
  }

  const char *rangeNames[] = {"24h", "7d", "30d", "365d"};
  display.print(rangeNames[currentRange]);

  if (timeOffset > 0) {
    display.print(" (-");
    display.print(timeOffset);
    switch (currentRange) {
    case RANGE_DAILY:
      display.print("d");
      break;
    case RANGE_WEEKLY:
      display.print("w");
      break;
    case RANGE_MONTHLY:
      display.print("m");
      break;
    case RANGE_YEARLY:
      display.print("y");
      break;
    }
    display.print(")");
  }

  // Get data
  float values[120];
  int count;
  getSeries(values, &count, isTemperature, currentRange, timeOffset);

  if (count < 2) {
    display.setCursor(10, 28);
    display.print("Not enough data");
    display.display();
    return;
  }

  // Graph dimensions
  int graphHeight = 42;
  int graphTop = 20;
  int graphWidth = 120;
  int graphLeft = 5;

  // Find min/max and calculate mean
  float minVal = values[0];
  float maxVal = values[0];
  float sum = 0;
  for (int i = 0; i < count; i++) {
    if (values[i] < minVal)
      minVal = values[i];
    if (values[i] > maxVal)
      maxVal = values[i];
    sum += values[i];
  }
  float mean = sum / count;

  // Improved scaling for temperature graph (mean-centered)
  if (isTemperature) {
    float dataSpan = maxVal - minVal;
    float span = max(2.0f, dataSpan * 1.2f);

    minVal = mean - (span / 2.0f);
    maxVal = mean + (span / 2.0f);
  } else {
    // Humidity: use simple min/max with padding
    if (maxVal - minVal < 0.1) {
      maxVal = minVal + 1;
    }
  }

  // Draw border
  display.drawRect(graphLeft - 1, graphTop - 1, graphWidth + 2, graphHeight + 2,
                   SSD1306_WHITE);

  // Draw grid lines (every 0.5 units for temp, 5 for humidity)
  float gridStep = isTemperature ? 0.5 : 5.0;
  float firstGrid = ceil(minVal / gridStep) * gridStep;
  for (float gv = firstGrid; gv < maxVal; gv += gridStep) {
    int y = graphTop + graphHeight -
            (int)((gv - minVal) / (maxVal - minVal) * graphHeight);
    for (int x = graphLeft; x < graphLeft + graphWidth; x += 3) {
      display.drawPixel(x, y, SSD1306_WHITE);
    }
  }

  // Draw data line
  for (int i = 0; i < count - 1; i++) {
    int x1 = graphLeft + (i * graphWidth / count);
    int x2 = graphLeft + ((i + 1) * graphWidth / count);

    int y1 = graphTop + graphHeight -
             (int)((values[i] - minVal) / (maxVal - minVal) * graphHeight);
    int y2 = graphTop + graphHeight -
             (int)((values[i + 1] - minVal) / (maxVal - minVal) * graphHeight);

    display.drawLine(x1, y1, x2, y2, SSD1306_WHITE);
  }

  // Y-axis labels
  display.setCursor(0, 10);
  display.print(maxVal, 1);
  display.setCursor(0, graphTop + graphHeight - 6);
  display.print(minVal, 1);

  display.display();
}

void showStatusMessage(const char *msg, int x = 10, int y = 28,
                       int delayMs = 0) {
  if (!displayAvailable)
    return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(x, y);
  display.print(msg);
  display.display();
  if (delayMs > 0)
    delay(delayMs);
}

void enterDeepSleep(bool periodicWakeup = false) {
  if (periodicWakeup) {
    Serial.printf("Sleep %s\n", WAKEUP_LABELS[wakeupIntervalIdx]);
    backgroundReading = true;
  } else {
    Serial.println("Sleep...");
    backgroundReading = false;
  }
  Serial.flush();

  if (!periodicWakeup) {
    showStatusMessage("Sleeping...", 30, 28, 500);
  }

  if (displayAvailable) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
  }

  Wire.end();
  if (periodicWakeup) {
    esp_sleep_enable_timer_wakeup(WAKEUP_OPTIONS_MIN[wakeupIntervalIdx] * 60ULL * 1000000ULL);
  }

  setupWakeupSources();
  esp_deep_sleep_start();
}

// ========== Mode Handlers ==========

void refreshDisplay() {
  switch (currentMode) {
  case MODE_OVERVIEW:
    displayOverview();
    break;
  case MODE_GRAPH:
    drawGraph(graphShowTemp);
    break;
  case MODE_SETTINGS:
    if (pendingConfirm != CONFIRM_NONE) {
      displayConfirm();
    } else if (inTimeEditMode) {
      displayTimeEdit();
    } else if (inHistoryView) {
      displayHistory();
    } else {
      displaySettings();
    }
    break;
  default:
    break;
  }
}

// Encoder rotation: context-dependent navigation.
// Confirm dialog ignores rotation (Yes/No are picked via the buttons).
void handleRotation(int delta) {
  if (pendingConfirm != CONFIRM_NONE) {
    return;
  }

  switch (currentMode) {
  case MODE_OVERVIEW: {
    int p = (int)currentOverviewPage + delta;
    p %= OV_PAGE_COUNT;
    if (p < 0)
      p += OV_PAGE_COUNT;
    currentOverviewPage = (OverviewPage)p;
    Serial.printf("Overview page: %s\n",
                  currentOverviewPage == OV_CLOCK ? "Clock" : "Default");
    break;
  }
  case MODE_GRAPH:
    timeOffset -= delta;
    if (timeOffset < 0)
      timeOffset = 0;
    if (timeOffset > 100)
      timeOffset = 100;
    Serial.printf("Graph offset: %d\n", timeOffset);
    break;
  case MODE_SETTINGS:
    if (inTimeEditMode) {
      adjustTimeEditField(delta);
    } else if (inHistoryView) {
      historyIndex -= delta;
      if (historyIndex < 0)
        historyIndex = 0;
      if (historyIndex >= (int)flashEntryCount)
        historyIndex = flashEntryCount - 1;
      Serial.printf("History: %d/%lu\n", historyIndex + 1, flashEntryCount);
    } else {
      settingsIndex += delta;
      if (settingsIndex < 0)
        settingsIndex = SETTINGS_COUNT - 1;
      if (settingsIndex >= SETTINGS_COUNT)
        settingsIndex = 0;
    }
    break;
  default:
    break;
  }
  refreshDisplay();
}

// Encoder click: context action only.
// Mode cycling moved to the dedicated mode button. This handler also services
// the Yes branch of the confirmation dialog.
void handleButtonPress() {
  if (pendingConfirm != CONFIRM_NONE) {
    ConfirmAction action = pendingConfirm;
    pendingConfirm = CONFIRM_NONE;
    switch (action) {
    case CONFIRM_NTP_SYNC:
      showStatusMessage("Syncing NTP...");
      if (syncTimeWithNTP()) {
        showStatusMessage("NTP Sync OK!", 20, 28, 1000);
      } else {
        showStatusMessage("NTP Sync Failed", 10, 28, 1000);
      }
      break;
    case CONFIRM_CLEAR_DATA:
      clearHistory();
      showStatusMessage("Data Cleared!", 10, 28, 800);
      break;
    default:
      break;
    }
    refreshDisplay();
    return;
  }

  if (currentMode == MODE_SETTINGS && inTimeEditMode) {
    timeEditField = (timeEditField + 1) % TE_FIELD_COUNT;
    refreshDisplay();
    return;
  }

  if (currentMode == MODE_SETTINGS && inHistoryView) {
    inHistoryView = false;
    refreshDisplay();
    return;
  }

  switch (currentMode) {
  case MODE_OVERVIEW:
    // No-op; mode cycling lives on the mode button.
    break;

  case MODE_GRAPH: {
    // Cycle 8 combos: Temp Day -> Week -> Month -> Year ->
    // Humid Day -> Week -> Month -> Year -> wrap.
    if (currentRange == RANGE_YEARLY) {
      currentRange = RANGE_DAILY;
      graphShowTemp = !graphShowTemp;
    } else {
      currentRange = (TimeRange)(currentRange + 1);
    }
    timeOffset = 0;
    static const char *RANGE_NAMES[] = {"Daily", "Weekly", "Monthly", "Yearly"};
    Serial.printf("Graph: %s %s\n", graphShowTemp ? "Temp" : "Humid",
                  RANGE_NAMES[currentRange]);
    break;
  }

  case MODE_SETTINGS:
    switch (settingsIndex) {
    case SET_NTP_SYNC:
      pendingConfirm = CONFIRM_NTP_SYNC;
      confirmEnterMs = millis();
      lastConfirmRedraw = 0;
      break;
    case SET_MANUAL_TIME:
      enterTimeEditMode();
      break;
    case SET_SLEEP:
      sleepTimeoutIdx = (sleepTimeoutIdx + 1) % SLEEP_OPTIONS_COUNT;
      Serial.printf("Sleep: %s\n", SLEEP_LABELS[sleepTimeoutIdx]);
      saveSettings();
      break;
    case SET_WAKEUP:
      wakeupIntervalIdx = (wakeupIntervalIdx + 1) % WAKEUP_OPTIONS_COUNT;
      Serial.printf("Wakeup: %s\n", WAKEUP_LABELS[wakeupIntervalIdx]);
      saveSettings();
      break;
    case SET_HISTORY:
      inHistoryView = true;
      historyIndex = flashEntryCount > 0 ? (int)flashEntryCount - 1 : 0;
      Serial.println("History view: open");
      break;
    case SET_CLEAR_DATA:
      pendingConfirm = CONFIRM_CLEAR_DATA;
      confirmEnterMs = millis();
      lastConfirmRedraw = 0;
      break;
    }
    break;

  default:
    break;
  }

  refreshDisplay();
}

// Encoder long-press: now only used to save and exit time-edit. All other
// long-press semantics retired when the mode button took over top-level nav.
void handleLongPress() {
  if (pendingConfirm != CONFIRM_NONE) {
    return;
  }
  if (currentMode == MODE_SETTINGS && inTimeEditMode) {
    saveTimeEdit();
    inTimeEditMode = false;
    showStatusMessage("Time Saved!", 25, 28, 600);
    refreshDisplay();
  }
}

// Dedicated mode button (GPIO 5) short-press:
// cycles Overview -> Graph -> Settings -> Overview. Inside a confirm prompt
// it acts as "No" and dismisses the prompt without cycling.
void handleModeButtonPress() {
  if (pendingConfirm != CONFIRM_NONE) {
    Serial.println("Confirm: cancelled (No)");
    pendingConfirm = CONFIRM_NONE;
    refreshDisplay();
    return;
  }

  currentMode = (DisplayMode)((currentMode + 1) % MODE_COUNT);

  switch (currentMode) {
  case MODE_OVERVIEW:
    Serial.println("Mode: Overview");
    break;
  case MODE_GRAPH:
    Serial.println("Mode: Graph");
    timeOffset = 0;
    currentRange = RANGE_DAILY;
    graphShowTemp = true;
    break;
  case MODE_SETTINGS:
    Serial.println("Mode: Settings");
    settingsIndex = 0;
    settingsScroll = 0;
    inTimeEditMode = false;
    inHistoryView = false;
    pendingConfirm = CONFIRM_NONE;
    break;
  default:
    break;
  }
  refreshDisplay();
}

// Dedicated mode button long-press: force deep sleep (periodic wake stays
// armed). Suppressed inside confirm prompts.
void handleModeLongPress() {
  if (pendingConfirm != CONFIRM_NONE) {
    return;
  }
  Serial.println("Mode button long-press: forcing sleep");
  enterDeepSleep(true);
}

// ========== Setup ==========

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  delay(2000); // Give USB time to reconnect
  Serial.flush();
#else
  delay(100);
#endif

  // Apply timezone from POSIX TZ string. All localtime_r() / mktime() calls
  // and Arduino's getLocalTime() will use this to compute local time with DST.
  setenv("TZ", TZ_STRING, 1);
  tzset();

  Serial.println("\n============================");
  Serial.println("ESP32-C3 Room Monitor");
  Serial.println("============================");

  bootCount++;
  Serial.printf("Boot: %d  CPU: %dMHz\n\n", bootCount, getCpuFrequencyMhz());

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  switch (wakeup_reason) {
  case ESP_SLEEP_WAKEUP_GPIO:
    Serial.println("Wakeup: User interaction");
    backgroundReading = false;
    break;
  case ESP_SLEEP_WAKEUP_TIMER:
    Serial.println("Wakeup: Periodic timer");
    backgroundReading = true;
    break;
  case ESP_SLEEP_WAKEUP_UNDEFINED:
    Serial.println("Wakeup: Power on/Reset");
    backgroundReading = false;
    Serial.println("Cold boot detected: Forcing NTP sync...");
    prefs.begin("settings", false);
    prefs.putULong("lastNtpSync", 0);
    prefs.end();
    lastNtpSync = 0;
    break;
  default:
    Serial.println("Wakeup: Power on/Reset");
    backgroundReading = false;
    break;
  }

  // Load settings from flash
  loadSettings();

  // Init I2C and display early for status messages
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(WIRE_SPEED);

  setupEncoder();

  if (!backgroundReading) {
    initDisplay();
  }

  // WiFi/NTP sync with display status
  if (shouldSyncNtp()) {
    if (displayAvailable) {
      display.clearDisplay();
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.print("Connecting WiFi...");
      display.setCursor(0, 12);
      display.print(WIFI_SSID);
      display.display();
    }
    printWiFiStatus();
    bool ntpSuccess = syncTimeWithNTP();

    showStatusMessage(ntpSuccess ? "NTP Sync OK!" : "NTP Sync Failed", 0, 28,
                      1000);
    // Only set compile time if never synced and NTP failed
    if (!ntpSuccess && lastNtpSync == 0) {
#ifdef COMPILE_TIME
      Serial.println("Using compile time (UTC)");
      rtc.setTime(COMPILE_TIME);
      lastNtpSync = rtc.getEpoch();
      saveSettings();
#else
      Serial.println("Using default time");
      rtc.setTime(0, 0, 12, 16, 1, 2026);
#endif
    }
  }
  if (!timeIsSane()) {
#ifdef COMPILE_TIME
    rtc.setTime(COMPILE_TIME);
    lastNtpSync = rtc.getEpoch();
    saveSettings();
#else
    rtc.setTime(0, 0, 12, 16, 1, 2026);
#endif
  }

  initSensor();

  loadRamBuffer();

  readAndLogSensor();

  if (backgroundReading) {
    Serial.println("Background complete");
    enterDeepSleep(true);
  }

  displayOverview();

  lastActivityTime = millis();
  historyIndex = flashEntryCount > 0 ? flashEntryCount - 1 : 0;
  timeOffset = 0;
  currentMode = MODE_OVERVIEW;
  hasLiveData = false;
  lastLiveUpdate = millis();
  lastClockRedraw = millis();
}

// ========== Loop ==========

void loop() {
  // Mode button (GPIO 5) handling
  if (modeLongPress) {
    modeLongPress = false;
    handleModeLongPress();
    lastActivityTime = millis();
  }
  if (modeButtonPressed) {
    modeButtonPressed = false;
    handleModeButtonPress();
    lastActivityTime = millis();
  }

  // Encoder click handling
  if (longPress) {
    longPress = false;
    handleLongPress();
    lastActivityTime = millis();
  }

  if (buttonPressed) {
    buttonPressed = false;
    handleButtonPress();
    lastActivityTime = millis();
  }

  // Encoder rotation with hysteresis
  int ticksSnapshot;
  noInterrupts();
  ticksSnapshot = encoderTicks;
  interrupts();

  int deltaTicks = ticksSnapshot - lastProcessedTicks;
  if (abs(deltaTicks) >= ENCODER_DETENTS_PER_CLICK) {
    int steps = deltaTicks / ENCODER_DETENTS_PER_CLICK;
    lastProcessedTicks += steps * ENCODER_DETENTS_PER_CLICK;
    encoderPos += steps;

    Serial.printf("[ENC] Ticks: %d, Steps: %+d, Pos: %d\n", ticksSnapshot,
                  steps, encoderPos);

    handleRotation(steps);
    lastActivityTime = millis();
  }

  // Confirm-prompt tick: redraw the countdown bar and auto-cancel after
  // CONFIRM_TIMEOUT_MS. Inactivity-sleep timer is NOT reset here.
  if (pendingConfirm != CONFIRM_NONE) {
    unsigned long nowMs = millis();
    if (nowMs - confirmEnterMs >= CONFIRM_TIMEOUT_MS) {
      Serial.println("Confirm: timed out (No)");
      pendingConfirm = CONFIRM_NONE;
      refreshDisplay();
    } else if (nowMs - lastConfirmRedraw > 200) {
      lastConfirmRedraw = nowMs;
      refreshDisplay();
    }
  }

  // Live sensor + clock updates in overview mode
  if (currentMode == MODE_OVERVIEW) {
    unsigned long nowMs = millis();
    if (nowMs - lastLiveUpdate > 5000) {
      if (readSensorLive(liveData)) {
        hasLiveData = true;
      }
      lastLiveUpdate = nowMs;
    }
    // While the clock page is sliding, refresh ~30 fps; otherwise once a sec.
    unsigned long redrawInterval =
        (currentOverviewPage == OV_CLOCK && minuteSliding) ? 33 : 1000;
    if (nowMs - lastClockRedraw > redrawInterval) {
      displayOverview();
      lastClockRedraw = nowMs;
    }
  }

  // Sleep after inactivity
  if (millis() - lastActivityTime > SLEEP_OPTIONS_MS[sleepTimeoutIdx]) {
    enterDeepSleep(true);
  }

  yield();
}
