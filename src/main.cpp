#include "config.h"
#include "esp32-hal.h"
#include "esp_sleep.h"
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
#define I2C_SDA_PIN 6
#define I2C_SCL_PIN 7

// Display Settings
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

// Configuration
#define SLEEP_TIMEOUT 30000
#define WIRE_SPEED 400000
#define NTP_STALENESS_INTERVAL                                                 \
  86400 // 1 hour = 3600, 6 hours = 21600, 1 day = 86400,
#define RAM_BUFFER_SIZE 48
#define MAX_FLASH_ENTRIES 8760
#define PERIODIC_WAKEUP_MINUTES 30
#define ENCODER_DEBOUNCE_MS 5 // Faster debounce, but ISR only on CLK
#define ENCODER_DETENTS_PER_CLICK                                              \
  2 // Not used in new ISR; keep for future tuning

// Display modes
enum DisplayMode {
  MODE_OVERVIEW,
  MODE_HISTORY,
  MODE_GRAPH_TEMP,
  MODE_GRAPH_HUMID,
  MODE_SETTINGS
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

// Settings
bool isSummerTime = false;

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
volatile unsigned long lastButtonPress = 0;
volatile unsigned long buttonPressStart = 0;
volatile unsigned long lastEncoderTime = 0;
unsigned long lastActivityTime = 0;

// Filtered encoder state (in detent steps)
int encoderPos = 0; // logical position in steps
int lastProcessedTicks = 0;

// UI state
DisplayMode currentMode = MODE_OVERVIEW;
TimeRange currentRange = RANGE_DAILY;
int timeOffset = 0;
int historyIndex = 0;
int lastDisplayedPos = 0;

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
      lastButtonPress = now;
    } else if (pressDuration >= 500) {
      longPress = true;
      lastButtonPress = now;
    }
  }
  lastActivityTime = now;
}

// ========== Hardware Initialization ==========

void setupWakeupSources() {
  esp_deep_sleep_enable_gpio_wakeup((1ULL << ENCODER_SW_PIN) |
                                        (1ULL << ENCODER_CLK_PIN) |
                                        (1ULL << ENCODER_DT_PIN),
                                    ESP_GPIO_WAKEUP_GPIO_LOW);
}

void setupEncoder() {
  pinMode(ENCODER_SW_PIN, INPUT_PULLUP);
  pinMode(ENCODER_CLK_PIN, INPUT_PULLUP);
  pinMode(ENCODER_DT_PIN, INPUT_PULLUP);

  // Initial state
  lastEncoderState =
      (digitalRead(ENCODER_CLK_PIN) << 1) | digitalRead(ENCODER_DT_PIN);
  encoderTicks = 0;
  encoderPos = 0;
  lastProcessedTicks = 0;
  lastDisplayedPos = 0;

  // Only use CLK pin interrupt for rotation, FALLING edge
  attachInterrupt(digitalPinToInterrupt(ENCODER_CLK_PIN), encoderISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(ENCODER_SW_PIN), buttonISR, CHANGE);
}

// ========== Time & Settings Helpers ==========

time_t getLocalTime(time_t utc) {
  int offset = BASE_GMT_OFFSET_SEC;
  if (isSummerTime) {
    offset += DST_OFFSET_SEC;
  }
  return utc + offset;
}

void loadSettings() {
  prefs.begin("settings", true);
  isSummerTime = prefs.getBool("summerTime", false);
  lastNtpSync = prefs.getULong("lastNtpSync", 0);
  prefs.end();
  Serial.printf("Settings loaded: DST=%s, LastSync=%lu\n",
                isSummerTime ? "ON" : "OFF", lastNtpSync);
}

void saveSettings() {
  prefs.begin("settings", false);
  prefs.putBool("summerTime", isSummerTime);
  prefs.putULong("lastNtpSync", lastNtpSync);
  prefs.end();
  Serial.printf("Settings saved: DST=%s\n", isSummerTime ? "ON" : "OFF");
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

bool syncTimeWithNTP() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(1000); // Give it a full second to clear

  // Force the radio to stay awake and at full power
  WiFi.setSleep(false);

  Serial.printf("Connecting to: %s\n", WIFI_SSID);

  // Some C3 boards need the BSSID or a specific phase-in
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");

    // If we hit Status 4, it's a hard fail. Try one reset.
    if (WiFi.status() == 4 && attempts < 5) {
      Serial.println("\nHard fail detected. Resetting radio...");
      WiFi.disconnect();
      delay(500);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    attempts++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("\nFailed. Final Status: %d\n", WiFi.status());
    return false;
  }

  Serial.println("\nConnected! Syncing NTP...");
  configTime(0, 0, NTP_SERVER);

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10000)) {
    Serial.println("NTP Sync Failed");
    return false;
  }

  rtc.setTimeStruct(timeinfo);
  lastNtpSync = rtc.getEpoch();
  saveSettings();

  // Clean up to save power
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
    oldestTimestamp = 0;
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

void displayOverview() {
  if (!displayAvailable || ramBufferCount == 0)
    return;

  SensorData &data = hasLiveData ? liveData : ramBuffer[ramBufferCount - 1];

  display.clearDisplay();

  // Convert UTC to local time
  time_t localTime = getLocalTime(rtc.getEpoch());
  struct tm timeinfo;
  localtime_r(&localTime, &timeinfo);

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

  // Header
  display.setCursor(0, 0);
  display.printf("[#%d/%d]", historyIndex + 1, totalEntries);

  // Timestamp (convert UTC to local)
  time_t localTime = getLocalTime(data.timestamp);
  struct tm timeinfo;
  localtime_r(&localTime, &timeinfo);
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

  display.setCursor(0, 0);
  display.print("=== SETTINGS ===");

  display.setCursor(0, 20);
  display.print("Summer Time (DST):");

  display.setTextSize(2);
  display.setCursor(20, 35);
  display.print(isSummerTime ? "ON" : "OFF");

  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print("Turn=Toggle Btn=Save");

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

void enterDeepSleep(bool periodicWakeup = false) {
  if (periodicWakeup) {
    Serial.printf("Sleep %dmin\n", PERIODIC_WAKEUP_MINUTES);
    backgroundReading = true;
  } else {
    Serial.println("Sleep...");
    backgroundReading = false;
  }
  Serial.flush();

  if (displayAvailable && !periodicWakeup) {
    display.clearDisplay();
    display.setCursor(30, 28);
    display.setTextSize(1);
    display.print("Sleeping...");
    display.display();
    delay(500);
  }

  if (displayAvailable) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
  }

  Wire.end();

  if (periodicWakeup) {
    esp_sleep_enable_timer_wakeup(PERIODIC_WAKEUP_MINUTES * 60ULL * 1000000ULL);
  }

  setupWakeupSources();
  esp_deep_sleep_start();
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

  if (shouldSyncNtp()) {
    printWiFiStatus();
    bool ntpSuccess = syncTimeWithNTP();

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

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(WIRE_SPEED);

  // New encoder setup
  setupEncoder();

  if (!backgroundReading) {
    initDisplay();
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
  // Long press: cycle time range (in graph modes)
  if (longPress) {
    longPress = false;

    if (currentMode == MODE_GRAPH_TEMP || currentMode == MODE_GRAPH_HUMID) {
      currentRange = (TimeRange)((currentRange + 1) % 4);
      timeOffset = 0;

      const char *rangeNames[] = {"Daily", "Weekly", "Monthly", "Yearly"};
      Serial.printf("Range: %s\n", rangeNames[currentRange]);

      drawGraph(currentMode == MODE_GRAPH_TEMP);
    } else if (currentMode == MODE_SETTINGS) {
      // Long press in settings clears cache
      clearHistory();
      display.clearDisplay();
      display.setTextSize(1);
      display.setCursor(10, 24);
      display.print("Cache cleared");
      display.display();
      delay(800);
      displaySettings();
    }

    lastActivityTime = millis();
  }

  // Button press: cycle modes OR save in settings
  if (buttonPressed) {
    buttonPressed = false;

    if (currentMode == MODE_SETTINGS) {
      // Save and exit settings
      saveSettings();
      Serial.println("Settings saved, exiting to overview");
      currentMode = MODE_OVERVIEW;
      displayOverview();
    } else {
      // Cycle through modes
      currentMode = (DisplayMode)((currentMode + 1) % 5);

      Serial.print("Mode: ");
      switch (currentMode) {
      case MODE_OVERVIEW:
        Serial.println("Overview");
        displayOverview();
        break;
      case MODE_HISTORY:
        Serial.println("History");
        historyIndex = flashEntryCount > 0 ? flashEntryCount - 1 : 0;
        displayHistory();
        break;
      case MODE_GRAPH_TEMP:
        Serial.println("Temperature Graph");
        timeOffset = 0;
        currentRange = RANGE_DAILY;
        drawGraph(true);
        break;
      case MODE_GRAPH_HUMID:
        Serial.println("Humidity Graph");
        timeOffset = 0;
        currentRange = RANGE_DAILY;
        drawGraph(false);
        break;
      case MODE_SETTINGS:
        Serial.println("Settings");
        displaySettings();
        break;
      }
    }

    lastDisplayedPos = encoderPos;
    lastActivityTime = millis();
  }

  // Encoder rotation: apply hysteresis using ENCODER_DETENTS_PER_CLICK
  int ticksSnapshot;
  noInterrupts();
  ticksSnapshot = encoderTicks; // copy volatile safely
  interrupts();

  int deltaTicks = ticksSnapshot - lastProcessedTicks;

  // Only act when we've moved at least ENCODER_DETENTS_PER_CLICK ticks
  if (abs(deltaTicks) >= ENCODER_DETENTS_PER_CLICK) {
    int steps = deltaTicks / ENCODER_DETENTS_PER_CLICK; // integer division
    lastProcessedTicks +=
        steps * ENCODER_DETENTS_PER_CLICK; // consume those ticks

    encoderPos += steps; // logical position in steps
    int delta = steps;   // what UI cares about

    // Debug: show filtered movement
    Serial.printf("[ENC] Ticks: %d, Steps: %+d, Pos: %d\n", ticksSnapshot,
                  steps, encoderPos);

    switch (currentMode) {
    case MODE_OVERVIEW:
      // No scrolling in overview
      break;

    case MODE_HISTORY:
      // Scroll through individual entries
      historyIndex -= delta;
      if (historyIndex < 0)
        historyIndex = 0;
      if (historyIndex >= (int)flashEntryCount)
        historyIndex = flashEntryCount - 1;
      Serial.printf("History: %d/%lu\n", historyIndex + 1, flashEntryCount);
      displayHistory();
      break;

    case MODE_GRAPH_TEMP:
    case MODE_GRAPH_HUMID:
      // Scroll through time ranges
      timeOffset -= delta;
      if (timeOffset < 0)
        timeOffset = 0;
      if (timeOffset > 100)
        timeOffset = 100;
      Serial.printf("Graph offset: %d\n", timeOffset);
      drawGraph(currentMode == MODE_GRAPH_TEMP);
      break;

    case MODE_SETTINGS:
      // Toggle DST on any rotation
      isSummerTime = !isSummerTime;
      Serial.printf("DST toggled: %s\n", isSummerTime ? "ON" : "OFF");
      displaySettings();
      break;
    }

    lastDisplayedPos = encoderPos;
    lastActivityTime = millis();
  }

  // Live updates while awake in overview
  if (currentMode == MODE_OVERVIEW) {
    unsigned long nowMs = millis();
    if (nowMs - lastLiveUpdate > 5000) { // refresh sensor every 5s
      if (readSensorLive(liveData)) {
        hasLiveData = true;
      }
      lastLiveUpdate = nowMs;
    }
    if (nowMs - lastClockRedraw > 1000) { // redraw time every 1s
      displayOverview();
      lastClockRedraw = nowMs;
    }
  }

  // Sleep timeout
  if (millis() - lastActivityTime > SLEEP_TIMEOUT) {
    enterDeepSleep(true);
  }

  yield();
}