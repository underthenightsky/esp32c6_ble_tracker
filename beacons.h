// ─────────────────────────────────────────────────────────────────────────
// beacons.h — Beacon database
// Auto-generated from Iwayplus-beacons.xlsx
//
// To add/update beacons: edit this array. lx,ly are the "Local Coordinates"
// column (pixel/grid units, same space as the Python positioning tool).
// lat,lon are optional (0,0 if unknown) — used only for GPS estimate.
// ─────────────────────────────────────────────────────────────────────────
#pragma once

struct BeaconInfo {
  const char* name;   // must match the BLE advertised device name exactly
  int lx, ly;          // local coordinates (px)
  double lat, lon;      // global coordinates (0,0 if unknown)
  int floor;            // floor number
};

#define NUM_BEACONS 4

const BeaconInfo BEACONS[NUM_BEACONS] = {
  {"IW26020525", 21, 42, 28.550851174448226, 77.27148496821343, 0},
  {"IW26060986",  2, 41, 28.550849767258980, 77.27154351284213, 0},
  {"IW25030978", 20, 18, 28.550787088929460, 77.27148691968132, 0},
  {"IW25030952",  2, 18, 28.550786974909656, 77.27154414562500, 0},
};
