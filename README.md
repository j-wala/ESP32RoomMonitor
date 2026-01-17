# ESP32-C3 Room Monitor with Deep Sleep

A battery-powered room monitor that displays temperature, humidity, and pressure readings on an OLED screen. The device uses deep sleep to conserve battery and wakes up when the rotary encoder is pressed or rotated.

## Features

- **Deep Sleep Mode**: Enters deep sleep after 30 seconds of inactivity to save battery
- **Wake-up Sources**: GPIO wakeup from rotary encoder button press or rotation (ESP32-C3 compatible)
- **Sensor Readings**: BME280 for temperature, humidity, and pressure
- **OLED Display**: 128x64 SSD1306 display for data visualization
- **Data Logging**: Stores up to 50 sensor readings in RTC memory (persistent across sleep)
- **History View**: Scroll through logged readings using the rotary encoder
- **Serial Debug**: Outputs sensor readings to serial monitor
- **Time Sync**: Optional WiFi NTP sync or compile-time initialization
- **Manual Encoder**: Software-based rotary encoder reading (no PCNT hardware required)

## Hardware Requirements

- ESP32-C3 Development Board
- BME280 Temperature/Humidity/Pressure Sensor
- SSD1306 OLED Display (128x64)
- Rotary Encoder (with button)
- Pull-up resistors for I2C (if not built-in)

## Pin Configuration

### I2C Bus (BME280 & SSD1306)
- **SDA**: GPIO 8
- **SCL**: GPIO 9

### Rotary Encoder
- **CLK**: GPIO 4
- **DT**: GPIO 3
- **SW (Button)**: GPIO 2

**Note**: All encoder pins use internal pull-up resistors.

## Wiring Diagram

```
ESP32-C3          BME280 / SSD1306
--------          -----------------
GPIO 8 (SDA) ---> SDA
GPIO 9 (SCL) ---> SCL
3.3V         ---> VCC
GND          ---> GND

ESP32-C3          Rotary Encoder
--------          --------------
GPIO 4       ---> CLK
GPIO 3       ---> DT
GPIO 2       ---> SW (Button)
GND          ---> GND (common)
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
3. Displays current reading
4. After 30 seconds of inactivity, enters deep sleep with periodic wake-up enabled

### Wake-Up Behavior
- **Encoder Button Press**: Wakes device and cycles through display modes
- **Encoder Rotation**: Scrolls through history entries (only in History mode)
- **Timer Wake-Up (automatic)**: Every 15 minutes, device wakes up, reads sensor in background (display stays off), and goes back to sleep

### Display Modes (Cycle with Button Press)

#### 1. Current Reading Mode
Shows:
- Current time
- Temperature: `T: 23.4C`
- Humidity: `H: 45%`
- Pressure: `P: 1013hPa`
- Total log count

#### 2. History Mode
Shows:
- Entry indicator: `[#5/20]`
- Timestamp of entry
- Temperature, humidity, pressure from that reading
- **Rotate encoder** to scroll through entries

#### 3. Temperature Graph Mode
Shows:
- Graph title: "Temp History"
- Line chart of temperature readings over time
- Min/max values on Y-axis
- Shows up to 120 most recent data points

#### 4. Humidity Graph Mode
Shows:
- Graph title: "Humid History"
- Line chart of humidity readings over time
- Min/max values on Y-axis
- Shows up to 120 most recent data points

### Controls
- **Button Press**: Cycle through display modes (Current → History → Temp Graph → Humid Graph → Current...)
- **Rotate Encoder**: Scroll through history entries (only works in History mode)
- **No Interaction**: After 30 seconds, enters deep sleep with 15-minute periodic wake-up timer

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
