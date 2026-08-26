// ─────────────────────────────────────────────────────────────────────────
// imu_lis3dh.h — Minimal register-level LIS3DH wake-on-motion driver.
//
// We talk to it directly over I2C rather than pulling in a full
// accelerometer library, since all we need is: configure a motion
// threshold, route it to INT1, and read/clear INT1_SRC on wake.
//
// Wiring (this project's actual pinout on the XIAO ESP32-C6):
//   LIS3DH SDA -> D4 (GPIO22), SCL -> D5 (GPIO23)
//     (Wire.begin(D4, D5) in the main .ino)
//   LIS3DH I1 (primary interrupt) -> D2 (GPIO2) -> IMU_INT_GPIO in
//     the main .ino — confirmed within the C6's LP/RTC domain
//     (GPIO0-7), the only pins that can wake it from deep sleep.
// Power LIS3DH from the same 3.3V rail as the ESP32.
//
// I2C ADDRESS — confirmed against the SmartElex/SparkFun-layout
// breakout datasheet: default is 0x19 with the address jumper on the
// underside of the board left OPEN. It's only 0x18 if that jumper has
// been bridged. If unsure, run an I2C scanner sketch once to check
// which address responds before trusting this default.
// ─────────────────────────────────────────────────────────────────────────
#pragma once
#include <Wire.h>

#define LIS3DH_ADDR 0x19

#define LIS3DH_REG_CTRL1     0x20
#define LIS3DH_REG_CTRL2     0x21
#define LIS3DH_REG_CTRL3     0x22
#define LIS3DH_REG_CTRL4     0x23
#define LIS3DH_REG_CTRL5     0x24
#define LIS3DH_REG_INT1_CFG  0x30
#define LIS3DH_REG_INT1_SRC  0x31
#define LIS3DH_REG_INT1_THS  0x32
#define LIS3DH_REG_INT1_DUR  0x33

// Motion threshold — 1 LSB = 16mg at the +/-2g full scale used below.
// 0x10 (~256mg) is a moderate "definitely moved" trigger. Lower it for
// a more sensitive/twitchy trigger, raise it to require a firmer nudge
// before waking (fewer false wakes = better battery life, at the cost
// of missing very gentle movement).
#define LIS3DH_MOTION_THRESHOLD 0x10

static void lis3dhWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(LIS3DH_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static uint8_t lis3dhRead(uint8_t reg) {
  Wire.beginTransmission(LIS3DH_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((int)LIS3DH_ADDR, 1);
  if (Wire.available()) return Wire.read();
  return 0;
}

// Configure the LIS3DH for low-power wake-on-motion, routed to INT1.
// Safe to call every wake (idempotent) — the LIS3DH is powered
// continuously off the battery rail and unaffected by the ESP32's
// sleep state, but re-applying config here is cheap and self-healing.
static void imuArmMotionInterrupt() {
  lis3dhWrite(LIS3DH_REG_CTRL1, 0x2F); // 10Hz ODR, low-power mode, X/Y/Z enabled
  lis3dhWrite(LIS3DH_REG_CTRL2, 0x01); // high-pass filter on INT1 path (removes gravity/tilt drift)
  lis3dhWrite(LIS3DH_REG_CTRL3, 0x40); // route AOI1 (motion) interrupt to INT1 pin
  lis3dhWrite(LIS3DH_REG_CTRL4, 0x00); // +/-2g full scale
  lis3dhWrite(LIS3DH_REG_CTRL5, 0x08); // latch INT1 until INT1_SRC is read
  lis3dhWrite(LIS3DH_REG_INT1_THS, LIS3DH_MOTION_THRESHOLD);
  lis3dhWrite(LIS3DH_REG_INT1_DUR, 0x00); // trigger immediately, no minimum duration
  lis3dhWrite(LIS3DH_REG_INT1_CFG, 0x2A); // OR combination — wake on X/Y/Z "high" (motion) events
}

// Read INT1_SRC to unlatch the interrupt (required since LIR_INT1 is
// set above). Call this on every wake, whether or not the IMU was the
// cause, so a stale latch can never block a future wake.
static void imuClearInterrupt() {
  lis3dhRead(LIS3DH_REG_INT1_SRC);
}