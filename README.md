# ESP32-C6 BLE Tracker — Low-Power / IMU-Gated Edition

Targets roughly **10 days on a 1000mAh battery** for an asset that
moves ~2 hrs/day, by only running BLE/WiFi when something's actually
happening — not on a fixed timer.

## Trade-off vs the other firmware variants

This version does **not** serve its own local web page. It's asleep
almost all the time, so it can't reliably answer HTTP requests — it
only pushes to your `ble_tracker_server`. If you want the
always-reachable local page, use `esp32_ble_tracker` (or
`particle_ble_tracker`) instead. You can't have both always-on local
browsability and this level of battery life on the same design — the
whole power saving comes from the radios being off almost all the
time.

## How it works

**Stationary** (the default state):
- Deep sleep, ~10 µA
- LIS3DH accelerometer armed for wake-on-motion, ~2-5 µA
- Nothing scans, nothing transmits — total draw ~15 µA
- Optional heartbeat (`HEARTBEAT_INTERVAL_S`, default 15 min): wakes
  briefly to re-push the last known position so your dashboard's
  "last seen" doesn't go stale for hours during a long stationary
  spell. Cheap since it's infrequent and skips the BLE scan entirely.

**Moving** (triggered by the IMU's motion interrupt):
- BLE scan burst (`SCAN_BURST_MS`, ~4-5s) → one-shot position estimate
- Moved ≥ `MOVE_THRESHOLD_M` (default 5m) since the last push? →
  connect WiFi, push to the server
- Didn't move that far? → skip WiFi entirely this cycle
- Re-checks every `ACTIVE_INTERVAL_S` (10s) using **light sleep**
  between cycles, which preserves the WiFi association — so a
  continuously-moving asset doesn't pay a full reconnect cost every
  10 seconds, just periodic scan+push bursts
- After `MAX_STATIONARY_CONFIRMS` (default 3, ~30s) consecutive
  checks with no real movement, drops WiFi and returns to deep,
  IMU-gated sleep

## Hardware

- ESP32-C6 dev board
- LIS3DH accelerometer breakout (I2C)
- Wiring:
  - LIS3DH SDA/SCL → ESP32-C6 I2C pins
  - LIS3DH INT1 → `IMU_INT_GPIO` in the `.ino` (default `GPIO_NUM_3`)
    — **this must be GPIO0–7**, the only pins in the C6's LP/RTC
    domain that can wake it from deep sleep. This is a hardware
    constraint of the chip, not something you can change in software.
  - LIS3DH VIN/GND → same 3.3V rail as the ESP32

No accelerometer library needed — `imu_lis3dh.h` is a small hand-
rolled I2C register driver (the LIS3DH's register map is simple and
well-documented, so this avoids pulling in a full library for what's
really about 8 register writes).

## Libraries

Just **NimBLE-Arduino** (h2zero, tested with 2.x). Everything else
(WiFi, HTTPClient, deep/light sleep) is core to the ESP32 Arduino
package — make sure you have a recent-enough `arduino-esp32` core
version with ESP32-C6 support.

## Configure

At the top of the `.ino`:

```cpp
const char* WIFI_SSID       = "YOUR_WIFI_SSID";
const char* WIFI_PASS       = "YOUR_WIFI_PASSWORD";
const char* SERVER_HOST     = "https://tracker.yourdomain.com";
const char* SERVER_API_KEY  = "changeme";      // matches server's BLE_TRACKER_API_KEY
const char* DEVICE_ID       = "esp32c6-01";    // unique per device
```

And in `imu_lis3dh.h`, `LIS3DH_ADDR` (0x18 or 0x19 depending on your
board's SA0 pin state) and `LIS3DH_MOTION_THRESHOLD` (how firm a nudge
is needed to trigger a wake — lower = more sensitive/more frequent
wakes, higher = fewer false wakes but might miss gentle movement).

## The 10-day estimate — read this before you commit to a battery

| Period | Current | Basis |
|---|---|---|
| Stationary (deep sleep + IMU armed) | ~15 µA | Published ESP32-C6 deep sleep (~10µA) + LIS3DH standby (~2-5µA) figures |
| Moving (BLE scan + WiFi, light-sleep between cycles) | ~40-50 mA avg | Estimated from published component figures — WiFi connected+modem-sleep, BLE scan burst duty-cycled over the 10s active interval |

At 2 hrs/day moving: **~85-100 mAh/day → ~10-11 days on 1000mAh**
(before real-world derating for regulator efficiency and battery
discharge curve, which typically costs another 10-15%).

**This is a directional estimate, not a bench measurement of your
specific board.** The biggest source of uncertainty is the "moving"
state average current — it depends heavily on your WiFi AP's DTIM
interval, signal strength, and exactly how well light-sleep preserves
the WiFi association on your specific core version. Before finalizing
a battery/enclosure:

1. Flash this firmware and measure actual current draw with a
   multimeter (in series with the battery) or a USB power profiler,
   in both states
2. Adjust `ACTIVE_INTERVAL_S`, `MAX_STATIONARY_CONFIRMS`,
   `HEARTBEAT_INTERVAL_S`, and `LIS3DH_MOTION_THRESHOLD` based on what
   you actually measure and how the trade-off between battery life
   and tracking responsiveness feels for your use case
3. Re-run the math with your measured numbers before ordering
   batteries for a fleet of these

## Tuning cheat sheet

| Constant | Effect of increasing it |
|---|---|
| `MOVE_THRESHOLD_M` | Fewer pushes, more battery life, coarser tracking |
| `ACTIVE_INTERVAL_S` | Fewer scan/push cycles while moving, more battery life, less responsive tracking |
| `MAX_STATIONARY_CONFIRMS` | Longer before dropping back to deep sleep after motion stops, more conservative about "really stopped" |
| `HEARTBEAT_INTERVAL_S` | Less frequent stationary check-ins, better battery life, staler "last seen" during long idle periods |
| `LIS3DH_MOTION_THRESHOLD` | Firmer nudge needed to wake, fewer false wakes, might miss gentle movement |
