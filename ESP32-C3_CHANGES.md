# ESP32-C3 Compatibility Changes

This document outlines the changes made to make the room monitor compatible with ESP32-C3.

## Issues Fixed

### 1. **EXT0/EXT1 Wakeup Not Supported**
- **Problem**: ESP32-C3 doesn't support `esp_sleep_enable_ext0_wakeup()` and `esp_sleep_enable_ext1_wakeup()`
- **Solution**: Replaced with `esp_deep_sleep_enable_gpio_wakeup()` which is ESP32-C3 compatible
- **Changed in**: `setupWakeupSources()` function (line 89-94)

```cpp
// Old (ESP32 classic):
esp_sleep_enable_ext0_wakeup(ENCODER_SW_PIN, 0);
esp_sleep_enable_ext1_wakeup((1ULL << ENCODER_CLK_PIN) | (1ULL << ENCODER_DT_PIN), ESP_EXT1_WAKEUP_ANY_HIGH);

// New (ESP32-C3):
esp_deep_sleep_enable_gpio_wakeup(
  (1ULL << ENCODER_SW_PIN) | (1ULL << ENCODER_CLK_PIN) | (1ULL << ENCODER_DT_PIN),
  ESP_GPIO_WAKEUP_GPIO_LOW
);
```

### 2. **ESP32Encoder Library Not Compatible**
- **Problem**: ESP32Encoder requires PCNT hardware not available on ESP32-C3
- **Solution**: Implemented software-based rotary encoder reading using interrupts
- **Changed in**: 
  - Removed `ESP32Encoder` library dependency from `platformio.ini`
  - Removed `ESP32Encoder encoder;` object
  - Added manual encoder state tracking with `encoderPos` and `lastEncoderState`
  - Updated `encoderISR()` to manually decode quadrature signals
  - Modified `setup()` to initialize encoder state instead of library
  - Updated `loop()` to track encoder position changes manually

```cpp
// New encoder handling (lines 55-76):
volatile int encoderPos = 0;
volatile uint8_t lastEncoderState = 0;

void IRAM_ATTR encoderISR() {
  uint8_t clkState = digitalRead(ENCODER_CLK_PIN);
  uint8_t dtState = digitalRead(ENCODER_DT_PIN);
  uint8_t currentState = (clkState << 1) | dtState;
  
  if (lastEncoderState == 0b11) {
    if (currentState == 0b01) encoderPos++;
    else if (currentState == 0b10) encoderPos--;
  }
  
  lastEncoderState = currentState;
  lastActivityTime = millis();
}
```

### 3. **Time Initialization**

Added three methods for setting the time (in priority order):

**Method 1: WiFi NTP Sync (lines 31-35)**
```cpp
#define WIFI_SSID "YourWiFiSSID"        // Add your WiFi credentials
#define WIFI_PASSWORD "YourPassword"
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC 3600             // UTC+1 for Germany
#define DAYLIGHT_OFFSET_SEC 3600        // DST offset
```

- WiFi connects ONLY on first boot
- Syncs time from NTP server
- Immediately disconnects to save battery
- Function: `syncTimeWithNTP()` (lines 107-155)

**Method 2: Compile-Time Initialization**
- Uses `COMPILE_TIME` macro from build flags
- Set automatically by PlatformIO with `$UNIX_TIME`
- Fallback if WiFi is disabled (WIFI_SSID is empty)

**Method 3: Manual Fallback**
- Default: Jan 16, 2026 12:00:00
- Used only if both WiFi and compile-time fail

## How to Use

### To Enable WiFi Time Sync:
1. Edit `src/main.cpp` lines 31-32
2. Replace empty strings with your WiFi credentials:
   ```cpp
   #define WIFI_SSID "YourNetworkName"
   #define WIFI_PASSWORD "YourPassword"
   ```
3. Adjust timezone if needed (line 34-35)
4. Upload the code
5. WiFi will connect once, sync time, then disconnect forever

### To Use Compile-Time (No WiFi):
1. Leave `WIFI_SSID` empty (default)
2. Upload the code
3. Time will be set to when the code was compiled

## Updated Wakeup Behavior

- **Wakeup cause**: `ESP_SLEEP_WAKEUP_GPIO` (instead of EXT0/EXT1)
- **Serial message**: "Wakeup: GPIO (encoder or button)"
- All three pins (button + 2 encoder pins) can wake the device

## Files Modified

1. **platformio.ini**
   - Removed: `madhephaestus/ESP32Encoder@^0.11.6`
   - Added: Build flags for compile-time timestamp
   ```ini
   build_flags = 
     -D COMPILE_TIME=$UNIX_TIME
     -D COMPILE_DATE=\"$PIOENV\"
   ```

2. **src/main.cpp**
   - Added WiFi and NTP time sync
   - Replaced ESP32Encoder with manual implementation
   - Changed wakeup mechanism to GPIO-based
   - Added three-tier time initialization

3. **README.md**
   - Updated features list
   - Added WiFi configuration section
   - Documented time sync options

## Testing Checklist

- [ ] Code compiles without errors
- [ ] Device wakes from button press
- [ ] Device wakes from encoder rotation
- [ ] Encoder scrolling works in history mode
- [ ] Display shows sensor readings
- [ ] Serial output shows correct wakeup reasons
- [ ] Time is set correctly (check serial output)
- [ ] Device enters deep sleep after timeout
- [ ] Data persists across sleep cycles

## Troubleshooting

**If compilation still fails:**
- Make sure you have the latest ESP32 platform (6.3.1+)
- Check that WiFi library is available (should be built-in)
- Verify all library versions in platformio.ini

**If encoder doesn't work smoothly:**
- The software implementation may be less precise than hardware PCNT
- Try adjusting debouncing in the ISR if needed
- Check encoder wiring and pull-up resistors

**If time is wrong:**
- Check WiFi credentials
- Verify timezone offsets (GMT_OFFSET_SEC)
- Check serial monitor for NTP sync status
- Ensure compile time is recent if using that method

**If device won't wake from sleep:**
- Verify GPIO wakeup is supported on your exact ESP32-C3 variant
- Check that encoder pins have pull-up resistors
- Try pressing/holding button longer during first tests
