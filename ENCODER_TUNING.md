# Encoder Jitter Fix & Tuning Guide

## Changes Made

### 1. **Gray Code State Table**
Replaced simple edge detection with a proper state machine that validates all encoder transitions.

```cpp
const int8_t encStates[] = {
  0,  // 00 -> 00 no change
  -1, // 00 -> 01 CCW
  1,  // 00 -> 10 CW
  0,  // 00 -> 11 invalid
  // ... (16 states total)
};
```

**Why**: Filters invalid transitions caused by contact bounce.

### 2. **Increased Debounce Time**
- **Before**: 5ms
- **After**: 20ms (configurable)

**Why**: Mechanical encoders can bounce for 10-20ms. The old 5ms wasn't enough.

### 3. **Accumulator Filter**
Only registers a "click" after 2 detents (half of a mechanical detent cycle).

```cpp
encoderAccumulator += direction;
if (abs(encoderAccumulator) >= ENCODER_DETENTS_PER_CLICK) {
  encoderPos += (encoderAccumulator > 0) ? 1 : -1;
  encoderAccumulator = 0;
}
```

**Why**: Prevents partial/spurious clicks from registering as full movements.

### 4. **Debug Output**
Added encoder state logging:
```
[ENC] Pos: 5, Delta: +1, Acc: 0
```

**Why**: Helps diagnose if jitter is still occurring.

## Tuning Parameters

### If Encoder is TOO SENSITIVE (jittery, double-counts)

#### Option 1: Increase Debounce Time
```cpp
#define ENCODER_DEBOUNCE_MS 30  // Try 25-40ms
```

#### Option 2: Increase Detents Per Click
```cpp
#define ENCODER_DETENTS_PER_CLICK 4  // Full detent cycle
```

### If Encoder is TOO SLUGGISH (misses clicks, feels laggy)

#### Option 1: Decrease Debounce Time
```cpp
#define ENCODER_DEBOUNCE_MS 10  // Try 10-15ms
```

#### Option 2: Decrease Detents Per Click
```cpp
#define ENCODER_DETENTS_PER_CLICK 1  // Every detent counts
```
**Warning**: Setting to 1 may bring back jitter.

## Testing Procedure

### 1. Watch Serial Monitor
Upload the code and open serial monitor at 115200 baud.

### 2. Rotate Encoder Slowly
**Expected output**:
```
[ENC] Pos: 1, Delta: +1, Acc: 0
[ENC] Pos: 2, Delta: +1, Acc: 0
[ENC] Pos: 3, Delta: +1, Acc: 0
```

**Bad (jitter)**:
```
[ENC] Pos: 1, Delta: +1, Acc: 0
[ENC] Pos: 2, Delta: +1, Acc: 0
[ENC] Pos: 1, Delta: -1, Acc: 0  ← Jumped back
[ENC] Pos: 2, Delta: +1, Acc: 0
```

**Bad (missed clicks)**:
```
Rotate 5 clicks...
[ENC] Pos: 1, Delta: +1, Acc: 0
[ENC] Pos: 2, Delta: +1, Acc: 0  ← Only 2 registered
```

### 3. Rotate Encoder Quickly
Should still register all clicks without skipping or bouncing.

### 4. Test in Each Mode
- **Overview**: Rotation should do nothing (correct)
- **History**: Each click scrolls one entry
- **Graphs**: Each click changes time offset

## Common Encoder Types

### Type 1: Full-Step Encoder (20 pulses/revolution)
- **Detents**: 20 per rotation
- **Recommended**: `ENCODER_DETENTS_PER_CLICK 4`
- **Debounce**: 20ms

### Type 2: Half-Step Encoder (40 pulses/revolution)
- **Detents**: 20 per rotation, but 40 transitions
- **Recommended**: `ENCODER_DETENTS_PER_CLICK 2` (current setting)
- **Debounce**: 20ms

### Type 3: High-Quality Encoder (no bounce)
- **Recommended**: `ENCODER_DETENTS_PER_CLICK 1`
- **Debounce**: 5-10ms

## Hardware Fixes (if software doesn't help)

### Option 1: Add Hardware Capacitors
Add 0.01µF (10nF) capacitors between each encoder pin and GND:
```
Encoder CLK ---[10nF]--- GND
Encoder DT  ---[10nF]--- GND
```

Place capacitors **close to the encoder pins** on the ESP32 side.

**Why**: Filters high-frequency noise at the hardware level.

### Option 2: Add Pull-Up Resistors
If encoder has no internal pull-ups, add 10kΩ resistors:
```
3.3V ---[10kΩ]--- CLK pin
3.3V ---[10kΩ]--- DT pin
3.3V ---[10kΩ]--- SW pin
```

**Why**: Ensures clean HIGH state when encoder is open.

### Option 3: Use Schmitt Trigger IC
Add a 74HC14 Schmitt trigger buffer between encoder and ESP32.

**Why**: Creates hysteresis, eliminates bounce at the hardware level.

## Diagnostic Commands

### Check Current Settings
Look at these lines in your serial output on boot:
```cpp
// You can add this to setup():
Serial.printf("Encoder debounce: %dms\n", ENCODER_DEBOUNCE_MS);
Serial.printf("Detents per click: %d\n", ENCODER_DETENTS_PER_CLICK);
```

### Count Actual vs Expected
Rotate encoder 10 times and check serial output:
```
Expected: 10 position changes
Actual: [count them from serial output]
```

**If actual < expected**: Increase sensitivity (decrease debounce or detents)
**If actual > expected**: Decrease sensitivity (increase debounce or detents)

## Quick Reference Table

| Symptom | Likely Cause | Fix |
|---------|--------------|-----|
| Double/triple counts per click | Debounce too short | Increase `ENCODER_DEBOUNCE_MS` to 30-40 |
| Position jumps back and forth | Invalid transitions | Check wiring, add hardware caps |
| Misses clicks | Detents too high | Decrease `ENCODER_DETENTS_PER_CLICK` to 1-2 |
| Feels laggy | Detents too high | Decrease `ENCODER_DETENTS_PER_CLICK` |
| Works slowly but not fast | Debounce too long | Decrease `ENCODER_DEBOUNCE_MS` to 10-15 |

## Current Configuration

```cpp
#define ENCODER_DEBOUNCE_MS 20          // 20ms debounce
#define ENCODER_DETENTS_PER_CLICK 2     // Half-step mode
```

This should work well for most common KY-040 style encoders.

## Alternative: Switch to Polling

If ISR-based reading still has issues, you can switch to polling in `loop()`:

```cpp
// In loop(), not ISR
static uint8_t lastState = 0b11;
uint8_t currentState = (digitalRead(ENCODER_CLK_PIN) << 1) | digitalRead(ENCODER_DT_PIN);

if (currentState != lastState) {
  uint8_t index = (lastState << 2) | currentState;
  int8_t direction = encStates[index];
  if (direction != 0) {
    encoderPos += direction;
  }
  lastState = currentState;
  delay(5);  // Simple debounce
}
```

**Trade-off**: More responsive to real turns, but uses more CPU.

## Next Steps

1. **Upload** the current code
2. **Open serial monitor** (115200 baud)
3. **Rotate encoder** and watch `[ENC]` debug lines
4. **Tune** `ENCODER_DEBOUNCE_MS` and `ENCODER_DETENTS_PER_CLICK` if needed
5. **Report results**: Show serial output of 10 encoder clicks

---

**Current Status**: Encoder now uses robust Gray code state machine with 20ms debounce and 2-detent accumulator. This should eliminate most jitter issues.
