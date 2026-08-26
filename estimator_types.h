// ─────────────────────────────────────────────────────────────────────────
// estimator_types.h
//
// Arduino IDE auto-generates function prototypes and inserts them near the
// top of the sketch, before any struct defined later in the .ino — so any
// struct used in a function signature must live in a header that's
// #included at the top, not defined inline in the .ino body.
// ─────────────────────────────────────────────────────────────────────────
#pragma once

struct EstResult {
  float x, y;
  double lat, lon;
  bool haveGps;
  const char* conf;
  const char* motion;
  const char* b1Name;
  int b1Rssi;
  int nBeacons;
};
