/*
  MotionWakeSimpleTest.ino — minimal wake-on-motion -> BLE scan -> WiFi push
  (power-optimized variant)

  Deliberately simple, for testing the pipeline end to end:
    1. Deep sleep. LIS3DH wakes it on motion (D2).
    2. On wake: scan for nearby BLE devices for a few seconds (this
       will pick up your phone if its Bluetooth is on — phones usually
       show up as an anonymous rotating address rather than a name,
       which is fine, you can still watch its RSSI change as you move
       it closer/farther. If you want it to show up with a real name,
       install a free app like "nRF Connect" and use its BLE
       peripheral/advertiser feature to broadcast a named signal).
    3. Connect WiFi, POST the scan results (device list + one fresh
       accel reading) to your laptop as one JSON blob.
    4. Go straight back to deep sleep. No loop, no re-checking, no
       "did it really stop moving" state machine — one wake, one
       scan, one push, one sleep. If it's not going back to sleep,
       the bug is much easier to find in this shape than in the full
       tracker's active-loop logic.

  No beacons.h / estimator_types.h dependency — this doesn't do
  position triangulation at all, just reports what it saw.

  Wiring (confirmed): SDA->D4 (GPIO22), SCL->D5 (GPIO23),
  LIS3DH I1 -> D2 (GPIO2, in the C6's LP/RTC wake domain).
  Libraries: SparkFun LIS3DH, NimBLE-Arduino.

  --- POWER CHANGES vs. original ---
  1. LIS3DH ODR dropped 10Hz -> 1Hz (CTRL_REG1 0x2F -> 0x1F). The
     LPen (low-power) bit was already set, so this doesn't change
     operating mode, only sample rate. Adds up to ~1s of wake
     latency worst-case; bump back to 0x2F (10Hz) if that feels
     sluggish in testing.
  2. Added isolateUnusedPins(): on the ESP32-C6, only GPIO0-7 are in
     the LP_IO domain that stays powered for GPIO wakeup (RTC_PERIPH
     stays on for D2), so those are the only pins where a stray
     pull can leak current in deep sleep. D0/D1 are unused here, so
     they get isolated. D3/D6-D10 live in the HP GPIO domain, which
     is fully powered off in deep sleep regardless of firmware —
     nothing to isolate there.
  3. No change needed on the LIS3DH side beyond ODR: LPen was
     already enabled, and full power-down isn't usable since it
     would also disable the wake interrupt.
  4. Added phase timing: millis() timestamps at each stage boundary,
     printed as a table right before deep sleep. Everything from
     "sensor read" onward is measured exactly. Wake+boot latency
     (interrupt -> setup() actually running) can't be measured this
     way -- millis() itself resets at boot -- so WAKE_MARKER_PIN
     (D6, unused) is driven HIGH as the very first line of setup()
     and LOW right before sleep, so that latency shows up directly
     on a scope or power profiler (e.g. Nordic PPK2) against the
     current trace instead of being guessed at.

  Note: XIAO ESP32C6 boards measure ~15uA bare deep-sleep current
  (dominated by the onboard LDO's own quiescent draw per community
  teardown measurements) — the LIS3DH in low-power mode typically
  adds a few uA on top of that per its datasheet. These firmware
  changes get you close to that floor; going lower generally means
  a different/lower-Iq regulator on the board itself, which is a
  hardware change, not a firmware one.
*/

#include "SparkFunLIS3DH.h"
#include "Wire.h"
#include <esp_sleep.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <HTTPClient.h>
#include "driver/rtc_io.h"

#include "beacons.h"
#include "estimator_types.h"

// ── WiFi / local test server ────────────────────────────────────────
const char* WIFI_SSID       = "Airtel_Iwayplus";
const char* WIFI_PASS       = "sulpyawi";
const char* SERVER_HOST     = "http://192.168.1.250:5000";  // your laptop's IP
const char* SERVER_API_KEY  = "f4d0cb00-dbf5-11f0-bd35-dd4e9bf51317";
const char* DEVICE_ID       = "esp32-01";

#define LIS3DH_MOTION_THRESHOLD 0x10   // MIDDLE GROUND: ~96mg ( 0x6), a firm tap/nudge triggers it,
                                        // but not every tiny desk vibration.

                                        // Production value was 0x10 (~256mg). Raise back toward
                                        // that once testing is done, if it's too sensitive in use.

// CTRL_REG1: ODR[3:0] | LPen | Zen | Yen | Xen
//   0x2F = 10Hz, low-power mode, all axes  (previous value)
//   0x1F =  1Hz, low-power mode, all axes  (current — lower sample rate,
//           same low-power mode, adds up to ~1s worst-case wake latency)
#define LIS3DH_CTRL_REG1_VALUE 0x1F

// Timing instrumentation ----------------------------------------------------
// WAKE_MARKER_PIN is toggled HIGH as the very first line of setup() and LOW
// right before esp_deep_sleep_start(). It's unused elsewhere on this board,
// so probing it with a scope or power profiler lines up the exact moment
// code execution starts against the current-draw trace -- the only way to
// see true wake+boot latency, since millis() resets at boot and can't
// measure anything before setup() runs.
#define WAKE_MARKER_PIN D6

#define SCAN_MS 1000              // how long to listen for BLE devices, set to 1500 or higher (not above 2500 to avoid high power drain) for better chances of locating the beacons and calculating the position

const float TEMP =3.0f;
static const int TOP_N =3;
static const float ALPHA_W =0.55f;
static const float BONUS =0.5f;
static const int MIN_RSSI = -110;
// not sure why there is max signal strength limit
// shouldnt stronger signal lead to more confident result ?
static const int MAX_RSSI =-55;

LIS3DH SensorOne(I2C_MODE, 0x19);
RTC_DATA_ATTR uint32_t g_bootCount = 0;
// Persistent estimator memory
// (so that we can compare currently estimated position with previous poistion and 
// decide wether to send the data or not  )
RTC_DATA_ATTR bool g_haveEma = false;
RTC_DATA_ATTR float g_emaX=0,g_emaY=0;
RTC_DATA_ATTR int8_t g_top2[2] {-1,-1};

// to check number of wakeups vs number of wifi pushes
RTC_DATA_ATTR uint32_t g_wakeCount = 0;
RTC_DATA_ATTR uint32_t g_noBeaconCount = 0;
RTC_DATA_ATTR uint32_t g_wifiFailCount = 0;
RTC_DATA_ATTR uint32_t g_pushOkCount = 0;

// Per-cycle phase timestamps. Plain (non-RTC) globals on purpose -- deep
// sleep is a full chip reset, so these zero out every boot, giving a clean
// per-cycle timing log instead of an accumulating total.
static unsigned long g_tBoot = 0;
static unsigned long g_tSensorReady = 0;
static unsigned long g_tBleDone = 0;
static unsigned long g_tPosDone = 0;
static unsigned long g_tWifiDone = 0;      // 0 if WiFi step was skipped/never finished
static unsigned long g_tPushDone = 0;      // 0 if push never happened this cycle

// Single Burst aggregated beacon readings
struct Agg{
  bool present = false;
  int peak = -200;
  int n =0;
  float pen_peak =0;
  float score=0;
};

static Agg g_agg[NUM_BEACONS];

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    // return if the bluetooth device dosen't have a name that ( which prevents us from checking it's name in the known beacons list)
  if(!dev->haveName()){ 
    return;
  }
  std::string name = dev->getName();
  // now we get the rssi and later return if the value exceeds the limits
  int rssi = dev->getRSSI();

  if(rssi <= MIN_RSSI ){
    return;
  }
  //now checking against the known beacons name
  for(int i =0; i<NUM_BEACONS;i++){
    if(name == BEACONS[i].name){
      g_agg[i].present = true;
      if(rssi > g_agg[i].peak){
        g_agg[i].peak = rssi;
      }

      g_agg[i].n++ ;
      return;
    }
  }
  }
};
static ScanCallbacks g_scanCallbacks;

// ── LIS3DH motion-interrupt setup (same as the confirmed-working test) ─
static void armMotionInterrupt() {
  SensorOne.writeRegister(LIS3DH_CTRL_REG1, LIS3DH_CTRL_REG1_VALUE);
  SensorOne.writeRegister(LIS3DH_CTRL_REG2, 0x01);

  // Reading REFERENCE resets the HPF's internal reference to whatever
  // the current (post-filter) value is. Without this, the filter needs
  // several ODR cycles to settle after being (re)enabled, and during
  // that window it can output a transient big enough to cross the
  // threshold by itself -- which looks exactly like a phantom wake
  // with the board sitting perfectly still.
  uint8_t ref;
  SensorOne.readRegister(&ref, LIS3DH_REFERENCE);
  delay(100); // let the filter fully settle before arming/clearing

  SensorOne.writeRegister(LIS3DH_CTRL_REG3, 0x40);
  SensorOne.writeRegister(LIS3DH_CTRL_REG4, 0x00);
  SensorOne.writeRegister(LIS3DH_CTRL_REG5, 0x08);
  SensorOne.writeRegister(LIS3DH_INT1_THS, LIS3DH_MOTION_THRESHOLD);
  SensorOne.writeRegister(LIS3DH_INT1_DURATION, 0x00);
  SensorOne.writeRegister(LIS3DH_INT1_CFG, 0x7F);
}

static void clearMotionInterrupt() {
  uint8_t dummy;
  SensorOne.readRegister(&dummy, LIS3DH_INT1_SRC);
}

// Isolate the GPIOs we don't use that actually matter for deep-sleep
// current on the C6: GPIO0-7 are the LP_IO pins whose power domain
// stays alive for our D2 (GPIO2) wake source. D0 (GPIO0) and D1
// (GPIO1) are unused here, so disconnect their input/output/pull
// circuitry before sleeping. D2 itself is left alone (it's the wake
// pin). D3/D6-D10 (GPIO16-21) are outside the LP_IO range — they sit
// in the HP GPIO domain, which is fully powered off in deep sleep on
// this chip, so there's no isolate() call for them and none needed.
static void isolateUnusedPins() {
  rtc_gpio_isolate(GPIO_NUM_0);  // D0, unused
  rtc_gpio_isolate(GPIO_NUM_1);  // D1, unused
}

static void doBleScan() {
  
  for(int i =0; i<NUM_BEACONS;i++){
    g_agg[i]= Agg();
  }

  NimBLEDevice::init("XIAO-C6-Tracker");
  // Give NimBLE host a brief moment to sync after deep sleep
unsigned long startSync = millis();

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&g_scanCallbacks, false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  // NimBLE-Arduino v2.x note: start() is NON-blocking -- it returns almost
  // immediately and the scan continues in the background. Calling deinit()
  // right after start() (as the original code did) tears the scan down
  // after only tens of ms, which is why beacons were never found. getResults()
  // is the v2.x function that actually blocks for the given duration; the
  // registered onResult() callback still fires per-device exactly as before.
  scan->getResults(SCAN_MS, false);   // blocks for SCAN_MS milliseconds
  NimBLEDevice::deinit(true);
}
static bool calculatePosition(EstResult& out) {
  int8_t order[NUM_BEACONS];
  int nPresent = 0;

  for (int i = 0; i < NUM_BEACONS; i++) {
    if (!g_agg[i].present) continue;
    float penalty = (g_agg[i].n == 1) ? -4.0f : (g_agg[i].n == 2 ? -2.0f : 0.0f);
    g_agg[i].pen_peak = g_agg[i].peak + penalty;
    g_agg[i].score = g_agg[i].pen_peak + ((i == g_top2[0] || i == g_top2[1]) ? BONUS : 0.0f);
    order[nPresent++] = i;
  }
  if (nPresent == 0) return false;

  // Insertion sort by score descending
  for (int a = 1; a < nPresent; a++) {
    int8_t key = order[a];
    float keyScore = g_agg[key].score;
    int b = a - 1;
    while (b >= 0 && g_agg[order[b]].score < keyScore) {
      order[b + 1] = order[b];
      b--;
    }
    order[b + 1] = key;
  }

  int topN = min(TOP_N, nPresent);
  float maxPen = g_agg[order[0]].pen_peak;
  float weights[8];
  float wsum = 0;

  for (int i = 0; i < topN; i++) {
    weights[i] = expf((g_agg[order[i]].pen_peak - maxPen) / TEMP);
    wsum += weights[i];
  }
  for (int i = 0; i < topN; i++) weights[i] /= wsum;

  float rx = 0, ry = 0;
  for (int i = 0; i < topN; i++) {
    rx += weights[i] * BEACONS[order[i]].lx;
    ry += weights[i] * BEACONS[order[i]].ly;
  }

  // Smooth position across deep sleep cycles using persistent RTC state
  if (!g_haveEma) {
    g_emaX = rx; g_emaY = ry; g_haveEma = true;
  } else {
    g_emaX = ALPHA_W * rx + (1.0f - ALPHA_W) * g_emaX;
    g_emaY = ALPHA_W * ry + (1.0f - ALPHA_W) * g_emaY;
  }

  g_top2[0] = order[0];
  g_top2[1] = (nPresent > 1) ? order[1] : (int8_t)-1;

  out.x = g_emaX; out.y = g_emaY;
  out.b1Name = BEACONS[order[0]].name;
  out.b1Rssi = g_agg[order[0]].peak;
  out.nBeacons = nPresent;
  out.conf = (nPresent >= 3) ? "high" : "low";
  out.motion = "walking";
  return true;
}

static bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) delay(50);
  return WiFi.status() == WL_CONNECTED;
}

static void pushPositionToServer(const EstResult& r, float ax, float ay, float az) {
  char buf[380];
  snprintf(buf, sizeof(buf),
    "{\"device_id\":\"%s\",\"x\":%.2f,\"y\":%.2f,\"conf\":\"%s\","
    "\"motion\":\"%s\",\"b1\":\"%s\",\"b1_rssi\":%d,\"n\":%d,"
    "\"accel\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}}",
    DEVICE_ID, r.x, r.y, r.conf, r.motion, r.b1Name, r.b1Rssi, r.nBeacons, ax, ay, az);

  HTTPClient http;
  String url = String(SERVER_HOST) + "/api/push?api_key=" + String(SERVER_API_KEY);
  if (http.begin(url)) {
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-Key", SERVER_API_KEY);
    http.POST(String(buf));
    http.end();
  }
}

// Prints the per-cycle phase breakdown. Called right before deep sleep,
// once all of this cycle's timestamps are known. tTeardownDone is passed
// in rather than read from a global since it's taken right at the call site.
static void printTimingSummary(unsigned long tTeardownDone) {
  Serial.println("\n--- Phase timing (ms) ---");
  Serial.printf("Sensor init+read : %4lu\n", g_tSensorReady - g_tBoot);
  Serial.printf("BLE scan         : %4lu\n", g_tBleDone - g_tSensorReady);
  Serial.printf("Position calc    : %4lu\n", g_tPosDone - g_tBleDone);

  unsigned long lastStamp = g_tPosDone;
  if (g_tWifiDone > 0) {
    Serial.printf("WiFi connect     : %4lu\n", g_tWifiDone - g_tPosDone);
    lastStamp = g_tWifiDone;
    if (g_tPushDone > 0) {
      Serial.printf("HTTP POST        : %4lu\n", g_tPushDone - g_tWifiDone);
      lastStamp = g_tPushDone;
    }
  } else {
    Serial.println("WiFi connect     : skipped (no beacons this cycle)");
  }

  Serial.printf("Teardown+isolate : %4lu\n", tTeardownDone - lastStamp);
  Serial.printf("Total setup() time: %4lu ms\n", tTeardownDone - g_tBoot);
  Serial.println("(wake+boot latency before setup() started is NOT included");
  Serial.println(" here -- probe WAKE_MARKER_PIN against the current trace");
  Serial.println(" on a scope/power profiler to see that part.)");
}

static void goToSleepUntilMotion() {
  armMotionInterrupt();
  clearMotionInterrupt();
  pinMode(D2, INPUT);
  esp_deep_sleep_enable_gpio_wakeup((1ULL << D2), ESP_GPIO_WAKEUP_GPIO_HIGH);

  // Safely shut down Wi-Fi only if it was turned on
if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  isolateUnusedPins();

  unsigned long tTeardownDone = millis();
  printTimingSummary(tTeardownDone);

  digitalWrite(WAKE_MARKER_PIN, LOW);  // marks "code execution done" for the scope/profiler

  Serial.println("Going back to sleep. Move the board to wake it.\n");
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  pinMode(WAKE_MARKER_PIN, OUTPUT);
  digitalWrite(WAKE_MARKER_PIN, HIGH);  // marks "code execution started" for the scope/profiler
  g_tBoot = millis();

  Serial.begin(115200);
  g_bootCount++;

  Wire.begin(D4, D5);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  const char* wakeReason = (cause == ESP_SLEEP_WAKEUP_GPIO) ? "motion"
                          : (cause == ESP_SLEEP_WAKEUP_UNDEFINED) ? "power_on"
                          : "other";
  Serial.printf("\nBoot #%lu, wake cause: %d (%s)\n", (unsigned long)g_bootCount, (int)cause, wakeReason);

//incrementing wake up counter
g_wakeCount++;

  if (SensorOne.begin() != 0) {
    Serial.println("LIS3DH not responding at 0x19 — halting.");
    while (true) delay(1000);
  }
  clearMotionInterrupt();  // clear any latch before doing anything else

  float ax = SensorOne.readFloatAccelX();
  float ay = SensorOne.readFloatAccelY();
  float az = SensorOne.readFloatAccelZ();
  Serial.printf("Accel: X=%.3f Y=%.3f Z=%.3f\n", ax, ay, az);
  g_tSensorReady = millis();

// Scan the area for known beacons
  Serial.printf("Scanning for BLE devices for %ds...\n", SCAN_MS);
  doBleScan();
  g_tBleDone = millis();

// estimate the position
EstResult result; //passing an object of EstResult type to the function
//it will make in place changes 
//and return a bool based on the success of the operation
bool validPos = calculatePosition(result);
g_tPosDone = millis();

//push if position calcualted is valid
if (validPos){
    Serial.println("Connecting to WiFi...");
  if (connectWiFi()) {
    g_tWifiDone = millis();
    Serial.print("Connected, IP: "); Serial.println(WiFi.localIP());
    pushPositionToServer(result,ax,ay,az);
    g_tPushDone = millis();
    // successfully pushed via WiFi
    g_pushOkCount++;
  } else {
    g_tWifiDone = millis();
    // couldnt send via WiFi
    g_wifiFailCount++;
    Serial.println("WiFi connect failed — skipping push this cycle.");
  }
}
else{

      // incrementing if no beacons get found, so no wifi pushup
    g_noBeaconCount++;
}

// to see what is happening
  Serial.printf("wake=%lu noBeacon=%lu wifiFail=%lu pushOk=%lu\n",
  g_wakeCount, g_noBeaconCount, g_wifiFailCount, g_pushOkCount);
  goToSleepUntilMotion();  // never returns
}

void loop() {
  // Unused — setup() always ends in deep sleep.
}
