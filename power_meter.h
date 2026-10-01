// power_meter.h — INA226 power metering + charge counter for the BLE tracker
// Add as a new tab in the Arduino IDE and #include "power_meter.h" in the main sketch.
// Replaces every INA226 snippet from earlier messages.
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <sys/time.h>
#include <INA226_WE.h>

// ── Settings ─────────────────────────────────────────────────────────
#define INA226_ADDR        0x40
#define INA_SHUNT_OHMS     0.1f     // R1 on the schematic
#define INA_MAX_AMPS       0.8f     // ±81.92 mV full-scale / 0.1 Ω ≈ 0.82 A
#define I_SLEEP_MA         0.040f   // deep-sleep draw at the BATTERY terminal; measure it
#define BOOT_OVERHEAD_MAH  0.0f     // wake→setup() latency charge; measure with WAKE_MARKER_PIN
#define INA_CORRECTION     1.0f     // scale factor vs your bench meter (ina226.setCorrectionFactor)

// INA226_WE >= 1.3 turned the old enum names (AVERAGE_16, ...) into compile errors and
// requires the INA226_ prefix. Older versions only have the unprefixed names.
#ifdef AVERAGE_16
  #define PM_AVG_16     INA226_AVERAGE_16
  #define PM_CONV_1100  INA226_CONV_TIME_1100
  #define PM_CONT       INA226_CONTINUOUS
#else
  #define PM_AVG_16     AVERAGE_16
  #define PM_CONV_1100  CONV_TIME_1100
  #define PM_CONT       CONTINUOUS
#endif

// ── State ────────────────────────────────────────────────────────────
INA226_WE ina226 = INA226_WE(INA226_ADDR);
static bool g_inaOk = false;

RTC_DATA_ATTR double g_usedMah = 0;            // charge drawn since the counter started
RTC_DATA_ATTR struct timeval g_sleepEnter;     // when we last went to sleep
RTC_DATA_ATTR bool g_haveSleepStamp = false;
static uint32_t g_prevMs = 0;                  // end of the last credited interval
static float    g_sleptS = 0;                  // length of the sleep we just woke from

struct PowerReading {
  float busV = NAN;       // VBUS pin = XIAO side of the shunt
  float battV = NAN;      // cell side = bus + shunt drop
  float currentMa = NAN;
  float powerMw = NAN;
};

// ── Charge counter ───────────────────────────────────────────────────
// The current profile is a staircase, so credit each interval with the reading
// taken at its END (not the average of two readings).
static void meterFeed(float ma) {
  uint32_t now = millis();
  g_usedMah += (double)ma * (now - g_prevMs) / 3600000.0;   // mA·ms → mAh
  g_prevMs = now;
}

// Call first thing in setup(): charges the sleep we just woke from, starts the interval clock.
static void meterWake(uint32_t bootMs) {
  g_prevMs = bootMs;
  g_sleptS = 0;
  if (g_haveSleepStamp) {
    struct timeval nowTv;
    gettimeofday(&nowTv, NULL);
    g_sleptS = (nowTv.tv_sec - g_sleepEnter.tv_sec) + (nowTv.tv_usec - g_sleepEnter.tv_usec) / 1e6f;
    if (g_sleptS < 0) g_sleptS = 0;
    g_usedMah += I_SLEEP_MA * g_sleptS / 3600.0 + BOOT_OVERHEAD_MAH;
  }
}

// ── INA226 ───────────────────────────────────────────────────────────
static bool inaBegin() {
  if (!ina226.init()) return false;
  ina226.setResistorRange(INA_SHUNT_OHMS, INA_MAX_AMPS);
  ina226.setAverage(PM_AVG_16);             // ~35 ms window: smooths WiFi bursts
  ina226.setConversionTime(PM_CONV_1100);
  ina226.setMeasureMode(PM_CONT);
  if (INA_CORRECTION != 1.0f) ina226.setCorrectionFactor(INA_CORRECTION);
  return true;
}

// Bounded wait for the first conversion (the library's own wait has no timeout).
static void inaWaitFirstConversion() {
  if (!g_inaOk) return;
  uint32_t t0 = millis();
  while (ina226.isBusy() && millis() - t0 < 100) delay(1);
}

// Full reading; also feeds the charge counter.
static PowerReading readPower() {
  PowerReading p;
  if (!g_inaOk) return p;
  float shuntMv = ina226.getShuntVoltage_mV();
  p.busV      = ina226.getBusVoltage_V();
  p.battV     = p.busV + shuntMv / 1000.0f;
  p.currentMa = ina226.getCurrent_mA();
  p.powerMw   = ina226.getBusPower();
  meterFeed(p.currentMa);
  return p;
}

// Cheap tick (one register read): call at the END of each constant-current phase.
static void meterTick() {
  if (g_inaOk) meterFeed(ina226.getCurrent_mA());
}

// Call right before isolateUnusedPins() in both sleep functions.
static void meterPrepareSleep() {
  meterTick();                                  // credit the awake time since the last tick
  if (g_inaOk) ina226.powerDown();              // ~2 µA instead of ~330 µA
  gettimeofday(&g_sleepEnter, NULL);
  g_haveSleepStamp = true;
}

// ── JSON helper: print null instead of "nan" (which is invalid JSON) ─
static void jnum(char* out, size_t n, float v, const char* fmt) {
  if (isnan(v)) snprintf(out, n, "null"); else snprintf(out, n, fmt, v);
}
