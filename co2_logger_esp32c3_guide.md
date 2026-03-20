# CO2 Logger ESP32-C3 Guide

## 1. Project Goal

Build a CO2 logger with dual-mode operation:

- Continuously log CO2, temperature, and humidity
- Prioritize low power usage on battery
- Provide live monitoring and historical plots when external power is available
- Keep timestamps with NTP when available, otherwise continue with the internal clock
- Store data locally in flash using a binary ring buffer

## 2. Core Principles

- The sensor stays on for measurement stability
- The ESP32-C3 sleeps aggressively while on battery
- External power unlocks Wi-Fi and the web interface
- Time is best-effort until NTP sync succeeds
- Data is always logged locally, even when network features are unavailable

## 3. Hardware Components

| Component | Selection / Notes |
| --- | --- |
| MCU | Seeed XIAO ESP32-C3 |
| Sensor | Sensirion SCD41 over I2C, address `0x62` |
| Battery | LiPo, about `140 mAh` |
| External power | `5V` input |
| Power detection | Voltage divider with `2 x 100k ohm` resistors |
| Power detect pin | `D0` |
| Storage | Internal flash |

## 4. Wiring

### 4.1 SCD41 to ESP32-C3

| SCD41 | ESP32-C3 |
| --- | --- |
| 3V | 3.3V |
| GND | GND |
| SDA | SDA |
| SCL | SCL |

### 4.2 Required jumper settings

- Power jumper -> `3V`
- I2C jumper -> `3V`
- Onboard LED -> disabled

### 4.3 5V detection circuit

```text
5V ---- 100k ---- D0 ---- 100k ---- GND
```

- `D0 HIGH` -> external power present
- `D0 LOW` -> battery mode
- `D0` is also used as a deep sleep wake source

## 5. Voltage Divider Final Spec

### Purpose

- Detect external `5V`
- Wake the ESP32-C3 from deep sleep
- Choose between battery mode and external power mode

### Electrical behavior

- Divider ratio: `1:2`
- `5V` input gives about `2.5V` on `D0`
- `0V` input gives `0V` on `D0`

This works because the ESP32-C3 GPIO high threshold is about `0.75 x Vcc`, which is roughly `2.4V` at `3.3V` supply. That makes `2.5V` a safe logic HIGH while staying below the GPIO voltage limit.

### Power draw

```text
I = 5V / (100k + 100k) = 25 uA
```

- This current is present whenever external `5V` is present
- That is acceptable for this design because it is not drawing from the battery-only path

### Design constraints

1. Use only an RTC-capable pin
   - `D0 = GPIO2`, which is valid for deep sleep wake
2. Configure wake on HIGH

```cpp
esp_sleep_enable_ext0_wakeup(GPIO_NUM_2, 1);
```

3. Do not enable internal pull-ups or pull-downs on `D0`
4. The divider itself defines the voltage and provides a clean transition when `5V` is unplugged

## 6. Operating Modes

### 6.1 Battery mode

Condition:

- `D0 == LOW`

Behavior:

- ESP32-C3 spends most of its time in deep sleep
- Wake interval is every `120 seconds`
- Sensor runs in low-power periodic measurement mode
- Each battery wake cycle stays awake for at least `15 seconds`
- Battery mode must not return to deep sleep until at least one valid sensor reading has been captured

Flow:

1. Wake
2. Start a `15 second` minimum awake timer
3. Read measurement until a valid sample is available
4. Log the first valid measurement to flash
5. If the valid measurement arrived before `15 seconds`, remain awake until `15 seconds` has elapsed
6. If no valid measurement arrives within `15 seconds`, remain awake until the first valid measurement is captured
7. Configure wake sources
8. Return to deep sleep

### 6.2 External power mode

Condition:

- `D0 == HIGH`

Behavior:

- ESP32-C3 stays awake
- Sensor runs in normal periodic measurement mode
- Measurement interval is every `10 seconds`
- Wi-Fi and web UI are enabled
- BLE may be added later if needed

### 6.3 External mode subcases

#### Home Wi-Fi available

- Connect automatically
- Perform NTP sync
- Serve the UI on the local network

#### No Wi-Fi available

- Start Access Point mode
- Allow direct user connection
- Keep the UI available without internet access

## 7. Web Interface

The web UI is available only when external power is present.

### Access methods

- Home Wi-Fi, when available
- Fallback AP mode, when Wi-Fi is unavailable

### Required features

- Live CO2, temperature, and humidity display
- Historical plots from flash logs
- Time status display:
  - synced
  - estimated
- Log download in `CSV` or `JSON`

### Recommended implementation

- Lightweight HTTP server
- Simple HTML and JavaScript frontend
- `Chart.js` for historical plotting

## 8. Time Strategy

### Time sources

1. NTP when Wi-Fi is available
   - Authoritative source
   - Sets the system clock
2. Internal RTC as fallback
   - Used during battery mode
   - Expected to drift over time

### Time quality flags

Each log entry should indicate time quality:

```cpp
enum TimeQuality {
    TIME_ESTIMATED = 1,
    TIME_SYNCED = 2
};
```

### Required behavior

- After NTP sync, timestamps are considered accurate
- During battery mode, the clock continues but may drift
- Logged records must preserve whether the time was synced or estimated

## 9. Data Logging

### Storage model

- Binary ring buffer in flash
- Fixed-size record layout
- Oldest records are overwritten when the buffer is full

### Log record structure

```cpp
struct LogEntry {
    uint32_t timestamp;         // Unix seconds
    uint16_t co2_ppm;
    int16_t temperature_x100;
    uint16_t humidity_x100;
    uint8_t flags;
    uint8_t reserved;
};
```

### Flag bits

- Bit 0 -> external power
- Bit 1 -> time synced
- Bit 2 -> time estimated
- Bit 3 -> valid reading

### Sampling rates

| Mode | Interval |
| --- | --- |
| Battery | `120 sec` |
| External power | `10 sec` |

### Capacity estimate

- Record size: about `12-16 bytes`
- Usable flash: about `1 MB`
- Estimated capacity: about `60k-80k` records
- Expected retention: weeks to months depending on operating mode

## 10. Firmware Flow

### 10.1 Boot or wake

1. Read `D0` for power detection
2. If HIGH, enter external power mode
3. If LOW, enter battery mode

### 10.2 Battery mode flow

```text
Wake
  -> Read sensor
  -> Log entry
  -> Configure wake sources:
       - timer (120s)
       - GPIO (D0 HIGH)
  -> Deep sleep
```

### 10.3 External power mode flow

```text
Stay awake
  -> Try Wi-Fi connection
  -> If success: NTP sync
  -> Else: start AP mode
  -> Every 10s:
       - Read sensor
       - Log entry
  -> Serve web UI
```

## 11. Power Considerations

### Expected consumption

| Component | Current |
| --- | --- |
| SCD41 | about `0.4 mA` |
| ESP32 average | about `0.7 mA` |

Estimated battery life in battery mode:

- About `4-5 days` in realistic conditions

### Critical optimizations

- Disable LED
- Minimize wake time
- Avoid Wi-Fi in battery mode

## 12. Software Stack

### Framework options

- Arduino for simpler development
- ESP-IDF for more control

### Library suggestions

| Area | Recommendation |
| --- | --- |
| SCD41 | Sensirion official library |
| Wi-Fi | ESP32 Wi-Fi stack |
| Web server | AsyncWebServer |
| Storage | Raw flash or partition-based ring buffer |

## 13. Important Constraints

### Sensor

- Do not power-cycle the SCD41 frequently
- Keep periodic measurement running

### Time

- Time is not reliable without NTP
- Every log must include time quality information

### Flash

- Flash write endurance is limited
- Use a ring buffer instead of append-only storage

## 14. Recommended System Behavior

- Battery mode logs every `2 minutes` with aggressive sleep
- External power mode logs every `10 seconds` and enables the UI
- Time is NTP-synced when possible and estimated otherwise
- All measurements are stored locally
- UI remains usable either through Wi-Fi or fallback AP mode

## 15. Design Challenge and Recommendation

Current assumption:

- Keeping the sensor always on is acceptable

Approved sensor mode plan:

- Battery mode -> SCD41 low-power periodic measurement
- External power mode -> SCD41 normal periodic measurement

Note:

- This mode switching is now part of the intended implementation plan, even though it may introduce some transient behavior during transitions.
- Validation should include checking whether the first one or two readings after a mode change need to be ignored or flagged.

## 16. Implementation Plan

Status note:

- Checked items below are based on completed implementation and direct hardware validation.
- Embedded unit tests were also used where hardware-independent storage behavior could be validated safely.
- Unchecked items either still need explicit runtime verification on battery/power hardware or a longer-duration stability/power measurement pass.

### Milestone 1: Hardware bring-up

- [x] Verify SCD41 wiring and I2C communication. Finish: `20 / 03 / 2026  01:42`
- [ ] Verify D0 voltage divider behavior with and without external 5V. Finish: `____ / ____ / ________  __:__`
- [ ] Disable LED and confirm baseline power draw. Finish: `____ / ____ / ________  __:__`
- [ ] Confirm deep sleep wake on timer and on D0 HIGH. Finish: `____ / ____ / ________  __:__`

### Milestone 2: Sensor and logging foundation

- [x] Initialize the SCD41 and validate stable CO2, temperature, and humidity readings. Finish: `20 / 03 / 2026  01:42`
- [x] Define and validate the `LogEntry` binary format. Finish: `20 / 03 / 2026  01:42`
- [x] Implement the flash ring buffer with overwrite behavior. Finish: `20 / 03 / 2026  01:42`
- [x] Confirm records can be written, read back, and parsed correctly. Finish: `20 / 03 / 2026  02:11`

### Milestone 3: Battery mode

- [x] Implement battery mode boot path when `D0 == LOW`. Finish: `20 / 03 / 2026  01:42`
- [ ] Switch the SCD41 into low-power periodic measurement mode in battery operation. Finish: `____ / ____ / ________  __:__`
- [x] Configure measurement and logging every `120 seconds`. Finish: `20 / 03 / 2026  01:42`
- [ ] Keep each battery wake cycle awake for at least `15 seconds`. Finish: `____ / ____ / ________  __:__`
- [ ] Do not return to deep sleep in battery mode until at least one valid sensor reading has been captured. Finish: `____ / ____ / ________  __:__`
- [ ] If a valid reading arrives before `15 seconds`, remain awake until the `15 second` minimum awake window has elapsed. Finish: `____ / ____ / ________  __:__`
- [ ] If no valid reading arrives within `15 seconds`, remain awake until the first valid reading is captured. Finish: `____ / ____ / ________  __:__`
- [x] Enter deep sleep after each logging cycle. Finish: `20 / 03 / 2026  01:42`
- [ ] Validate average current draw and expected battery life assumptions. Finish: `____ / ____ / ________  __:__`

### Milestone 4: External power mode and connectivity

- [x] Implement external power boot path when `D0 == HIGH`. Finish: `20 / 03 / 2026  01:42`
- [ ] Switch the SCD41 into normal periodic measurement mode in external-power operation. Finish: `____ / ____ / ________  __:__`
- [x] Connect to known Wi-Fi when available. Finish: `20 / 03 / 2026  09:59`
- [x] Add fallback AP mode when Wi-Fi is unavailable. Finish: `20 / 03 / 2026  01:42`
- [x] Perform NTP sync and mark time quality correctly in logs. Finish: `20 / 03 / 2026  09:59`

### Milestone 5: Web UI

- [x] Serve a local web page in external power mode. Finish: `20 / 03 / 2026  01:42`
- [x] Show live CO2, temperature, and humidity values. Finish: `20 / 03 / 2026  01:42`
- [x] Render historical plots from stored log data. Finish: `20 / 03 / 2026  01:42`
- [x] Add log export in `CSV` or `JSON`. Finish: `20 / 03 / 2026  01:42`

### Milestone 6: Validation and polish

- [ ] Test power transitions between battery and external supply. Finish: `____ / ____ / ________  __:__`
- [x] Verify time quality flags across synced and unsynced operation. Finish: `20 / 03 / 2026  01:42`
- [x] Check ring buffer behavior when storage wraps. Finish: `20 / 03 / 2026  02:11`
- [ ] Validate sensor readings immediately after switching between low-power periodic and normal periodic modes. Finish: `____ / ____ / ________  __:__`
- [ ] Review long-run stability, power usage, and sensor behavior. Finish: `____ / ____ / ________  __:__`

## 17. Acceptance Checklist

- [x] Logger records valid measurements in both operating modes
- [ ] Battery mode wakes on timer and external power insertion
- [ ] Battery mode stays awake for at least `15 seconds` per wake cycle
- [ ] Battery mode does not sleep until at least one valid measurement has been captured
- [x] External mode provides live UI access through Wi-Fi or AP
- [x] Timestamps are marked as synced or estimated
- [x] Historical data remains accessible after log buffer wrap
- [ ] Power behavior matches the intended low-power design
