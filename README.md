# ESP32-C3 Room Monitor with Deep Sleep

A battery-powered room monitor that displays temperature, humidity, and pressure readings on an OLED screen. The device uses deep sleep to conserve battery and wakes up when any of the buttons or the rotary encoder is touched.

## Features

- **Deep Sleep Mode**: Enters deep sleep after a configurable inactivity timeout (default 30 s)
- **Wake-up Sources**: GPIO wakeup from rotary encoder (CLK/DT/SW) and the dedicated mode button
- **Sensor Readings**: BME280 for temperature, humidity, and pressure
- **OLED Display**: 128x64 SSD1306 display with a default overview page and a clock-centric page (slide animation on minute change)
- **Data Logging**: Up to ~8760 sensor readings persisted to LittleFS (24 h working set in RAM)
- **History View**: Nested under Settings -> History; scroll with the rotary encoder
- **Graphs**: Encoder click cycles 8 combos (Temp/Humid x Daily/Weekly/Monthly/Yearly); rotation scrolls back in time
- **Settings auto-save**: Sleep / Wakeup interval persist immediately when changed
- **Confirm prompts**: NTP Sync and Clear Data ask first, with a 10 s countdown auto-cancel
- **Time Sync**: WiFi NTP sync (configurable in `include/config.h`) with timezone-aware DST handling
- **Manual Encoder**: Software-based rotary encoder reading (no PCNT hardware required)

## Hardware Requirements

- ESP32-C3 Development Board
- BME280 Temperature/Humidity/Pressure Sensor
- SSD1306 OLED Display (128x64)
- Rotary Encoder (with built-in button)
- Dedicated tactile push button (mode/wake)
- Pull-up resistors for I2C (if not built-in)

## Pin Configuration

### I2C Bus (BME280 & SSD1306)
- **SDA**: GPIO 6
- **SCL**: GPIO 7

### Rotary Encoder
- **CLK**: GPIO 4
- **DT**: GPIO 3
- **SW (Button)**: GPIO 2

### Mode / Wake Button
- **Switch**: GPIO 5 (active-low to GND)

**Note**: All encoder and button pins use internal pull-up resistors.

## Wiring Diagram

```
ESP32-C3          BME280 / SSD1306
--------          -----------------
GPIO 6 (SDA) ---> SDA
GPIO 7 (SCL) ---> SCL
3.3V         ---> VCC
GND          ---> GND

ESP32-C3          Rotary Encoder
--------          --------------
GPIO 4       ---> CLK
GPIO 3       ---> DT
GPIO 2       ---> SW (Button)
GND          ---> GND (common)

ESP32-C3          Mode Button
--------          -----------
GPIO 5       ---> one terminal
GND          ---> other terminal
```

## I2C Addresses

- **BME280**: 0x76 (default, can be 0x77 on some modules)
- **SSD1306**: 0x3C

If your BME280 uses address 0x77, change line 80 in `main.cpp`:
```cpp
if (!bme.begin(0x77, &Wire)) {  // Change from 0x76 to 0x77
```

## Operation

### Initial Power-On
1. Device initializes display and sensor
2. Takes first sensor reading
3. Displays the default overview page
4. After the configured inactivity timeout (default 30 s) it enters deep sleep with periodic wake-up enabled

### Wake-Up Behavior
- **Mode button (GPIO 5)**: Wakes the device; short press cycles top-level modes; long press forces sleep
- **Encoder click**: Wakes the device; in mode = context action (see below)
- **Encoder rotation**: Wakes the device; in mode = context navigation
- **Timer wake-up**: Every `Wakeup` interval (default 30 min) the device wakes silently, takes a reading, and returns to sleep

### Top-level modes (cycled by the mode button)

`Overview -> Graph -> Settings -> Overview ...`

#### 1. Overview
Two pages, switched by **rotating the encoder**:

- **Default page**: large temperature, date / time on top, humidity / pressure on the bottom row.
- **Clock page**: large `HH:MM` with a slide animation when the minute changes; date on top, `T:xx.xC  H:yy%` on the bottom row.

#### 2. Graph
Line chart of temperature or humidity over time.

- **Encoder click**: cycles all 8 combos `Temp Day -> Temp Week -> Temp Month -> Temp Year -> Humid Day -> ... -> Humid Year -> wrap` (resets time offset).
- **Encoder rotate**: scrolls the time window backwards (`(-1d)`, `(-2d)`, etc., units depend on range).

#### 3. Settings
Six items, navigated with the encoder, activated with an encoder click:

- **NTP Sync** -- opens a confirm prompt before launching the WiFi attempt.
- **Set Time** -- enters the manual time-edit sub-mode (turn = change field, click = next field, encoder long-press = save & exit).
- **Sleep** -- click cycles `15s / 30s / 1m / 2m / 5m`; auto-saved.
- **Wakeup** -- click cycles `10m / 15m / 30m / 1h`; auto-saved.
- **History** -- nested entry browser (turn to scroll, encoder click to exit).
- **Clear Data** -- opens a confirm prompt before wiping flash history.

### Confirm prompts

NTP Sync and Clear Data prompt with `Yes=Click  No=Mode` and a 10 s countdown bar. If the timer hits zero with no input, the prompt cancels itself.

### Controls summary

| Surface | Short press | Long press | Rotate |
|---|---|---|---|
| **Mode button (GPIO 5)** | Cycle top-level mode | Force deep sleep | -- |
| **Encoder click** | Context action (see modes) | Save in time-edit only | -- |
| **Encoder rotate** | Context navigation | -- | -- |

- **No interaction**: After the configured inactivity timeout, enters deep sleep with a periodic wake-up timer armed.

## Configuration Options

You can modify these constants in `main.cpp`:

```cpp
#define SLEEP_TIMEOUT 30000           // Sleep after 30 seconds (adjust as needed)
#define MAX_LOG_ENTRIES 50            // Maximum logged readings
#define PERIODIC_WAKEUP_MINUTES 15    // Background sensor reading interval
#define SCREEN_ADDRESS 0x3C           // OLED I2C address
```

**Periodic Wake-Up**: The device automatically wakes every 15 minutes to read the sensor without turning on the display. This builds a complete data history while maximizing battery life. Change `PERIODIC_WAKEUP_MINUTES` to adjust the interval.

### WiFi Configuration for Time Sync

**Option 1: WiFi NTP Sync (Recommended)**

To enable automatic time synchronization via WiFi, edit `main.cpp` lines 31-35:

```cpp
#define WIFI_SSID "YourWiFiName"
#define WIFI_PASSWORD "YourPassword"
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC 3600          // UTC+1 (adjust for your timezone)
#define DAYLIGHT_OFFSET_SEC 3600     // 1 hour for DST (0 if no DST)
```

**Important**: WiFi only connects on first boot to sync time, then immediately disconnects to save battery. The time is maintained by the RTC during deep sleep.

**Option 2: Compile-Time Initialization**

Leave `WIFI_SSID` empty (default). The RTC will be initialized with the compilation timestamp automatically:

```cpp
#define WIFI_SSID ""  // WiFi disabled - uses compile time
```

**Option 3: Manual Time Setting**

If both methods fail, a fallback time is used (currently Jan 16, 2026 12:00:00). You can change this in line 323 of `main.cpp`.

## Serial Monitor Output

Connect via USB at 115200 baud to see debug information:
```
=== ESP32-C3 Room Monitor ===
Boot count: 1
Wakeup: Power on/Reset
BME280 initialized successfully
=== Sensor Reading ===
Temperature: 23.45 °C
Humidity: 45.2 %
Pressure: 1013.25 hPa
Time: 2026-01-16 12:00:00
Log entries: 1
====================
```

## Power Consumption

- **Active Mode**: ~80-100mA (display on, sensors active)
- **Deep Sleep**: ~10-50µA
- **Wake-up Latency**: ~200-300ms

With 30-second timeout and typical usage (a few wake-ups per day), a 2000mAh battery should last several weeks.

## Troubleshooting

### Display Not Working
- Check I2C connections (SDA/SCL)
- Verify OLED address is 0x3C (use I2C scanner if needed)
- Ensure proper power supply (3.3V)

### Sensor Not Found
- Check BME280 I2C address (0x76 or 0x77)
- Verify I2C connections
- Check power connections

### Encoder Not Waking Device
- Verify pin connections (GPIO 6, 7, 8)
- Check that encoder has common ground with ESP32
- Encoder switch should be normally open

### Won't Enter Deep Sleep
- Check serial monitor for activity
- Verify no continuous encoder changes
- Check SLEEP_TIMEOUT setting

## Building and Uploading

Using PlatformIO:
```bash
pio run -t upload
pio device monitor
```

## Future Enhancements

- WiFi/NTP for automatic time synchronization
- MQTT publishing of sensor data
- Battery voltage monitoring
- Adjustable sleep timeout via encoder
- Data export over serial
- Multiple sensor support
- Alarm/threshold notifications

## License

MIT License - Feel free to modify and use for your projects.
