#include "motion.h"
#include "drivelog.h"
#include "obd.h"
#include "board.h"
#include "brain.h"
#include "settings.h"
#include "recorder.h"
#include "trip.h"
#include "audio.h"
#include "music.h"
#include "display.h"
#include <math.h>

MotionState g_motion;

// ------------------------------------------------------------ QMI8658 -----
static uint8_t s_addr = 0x6B;
enum : uint8_t {
  REG_WHOAMI = 0x00, REG_CTRL1 = 0x02, REG_CTRL2 = 0x03, REG_CTRL3 = 0x04, REG_CTRL5 = 0x06,
  REG_CTRL7 = 0x08, REG_TEMP_L = 0x33, REG_AX_L = 0x35, REG_RESET = 0x60,
};
static const float ACC_LSB = 8192.0f;  // +-4 g
static const float GYR_LSB = 64.0f;    // +-512 dps

static bool imuWrite(uint8_t r, uint8_t v) { return boardI2CWrite(s_addr, r, v); }

static bool imuConfigure(bool lowPower) {
  imuWrite(REG_CTRL7, 0x00);                       // sensors off while (re)configuring:
  delay(2);                                        // changing ODR on a running QMI8658 froze its data
  imuWrite(REG_CTRL1, 0x40);                       // register auto-increment
  if (lowPower) {
    imuWrite(REG_CTRL2, 0x18);                     // +-4 g, ~28 Hz
    imuWrite(REG_CTRL5, 0x00);                     // filters OFF: at this ODR the LPF rang to 2 g and
                                                   // then froze the output (sleep peeks saw no motion)
    imuWrite(REG_CTRL7, 0x01);                     // accel only
  } else {
    imuWrite(REG_CTRL2, 0x16);                     // +-4 g, ~112 Hz
    imuWrite(REG_CTRL3, 0x56);                     // +-512 dps, ~112 Hz
    imuWrite(REG_CTRL5, 0x11);                     // low-pass filters on
    imuWrite(REG_CTRL7, 0x03);                     // accel + gyro
  }
  return true;
}

static bool imuProbe() {
  const uint8_t addrs[] = {0x6B, 0x6A};
  for (uint8_t a : addrs) {
    uint8_t id = 0;
    if (boardI2CRead(a, REG_WHOAMI, &id, 1) && id == 0x05) { s_addr = a; return true; }
  }
  return false;
}

bool motionReadAccel(float& x, float& y, float& z) {
  uint8_t b[6];
  if (!boardI2CRead(s_addr, REG_AX_L, b, 6)) return false;
  x = (int16_t)(b[0] | b[1] << 8) / ACC_LSB;
  y = (int16_t)(b[2] | b[3] << 8) / ACC_LSB;
  z = (int16_t)(b[4] | b[5] << 8) / ACC_LSB;
  return true;
}

static bool readAll(float a[3], float g[3], float& temp) {
  uint8_t b[14];
  if (!boardI2CRead(s_addr, REG_TEMP_L, b, 14)) return false;
  temp = (int16_t)(b[0] | b[1] << 8) / 256.0f;
  for (int i = 0; i < 3; i++) {
    a[i] = (int16_t)(b[2 + 2 * i] | b[3 + 2 * i] << 8) / ACC_LSB;
    g[i] = (int16_t)(b[8 + 2 * i] | b[9 + 2 * i] << 8) / GYR_LSB;
  }
  return true;
}

void motionLowPower() { if (g_motion.ok) imuConfigure(true); }
// Out of wake-on-motion into low-power readings. On Otto's board the accel output stayed frozen at
// the value from before WoM was armed when only reconfigured (every sleep check saw "moved 0.001 g,
// vib 0", so tilting / the engine never woke him; bench `womcheck`, Nov 15 2026). A soft reset
// first gives live data again.
void motionLeaveWom() {
  if (!g_motion.ok) return;
  imuWrite(REG_RESET, 0xB0);
  delay(15);
  imuConfigure(true);
}
// vibcap: accel only at ~1 kHz ODR, filters off. Each sample is read when the chip flags new
// data (STATUS0 bit 0), so we sample at exactly the chip's own rate: pacing with our own clock
// instead beat against the ODR and made a fake 100 Hz peak. rateHz is unused (kept for the API).
// Blocks for n / ODR seconds. Returns samples taken; actualHz = measured sample rate.
// Restores the normal configuration afterwards.
int motionVibCapture(float* ax, float* ay, float* az, int n, float rateHz, float& actualHz) {
  actualHz = 0;
  if (!g_motion.ok) return 0;
  imuWrite(REG_CTRL7, 0x00);
  delay(2);
  imuWrite(REG_CTRL1, 0x40);
  imuWrite(REG_CTRL2, 0x13);                       // +-4 g, ~1 kHz
  imuWrite(REG_CTRL5, 0x00);                       // raw: no low-pass
  imuWrite(REG_CTRL7, 0x01);                       // accel only
  delay(200);                                      // output is junk for ~100 ms after a mode switch
  (void)rateHz;
  uint32_t t0 = micros();
  int got = 0;
  for (int i = 0; i < n; i++) {
    uint32_t w0 = micros();
    uint8_t st = 0;
    do { boardI2CRead(s_addr, 0x2E, &st, 1); } while (!(st & 0x01) && micros() - w0 < 5000);  // STATUS0.aDA
    float x, y, z;
    if (!motionReadAccel(x, y, z)) { x = y = z = 0; }
    ax[i] = x; ay[i] = y; az[i] = z;
    got++;
  }
  uint32_t el = micros() - t0;
  actualHz = el ? got * 1e6f / el : 0;
  motionInit();                                    // back to normal mode (resets + reconfigures)
  return got;
}

// Debug (imulp): low-power config with explicit CTRL2 / CTRL5 (0xFF = leave CTRL5 alone)
void motionLowPowerTest(uint8_t ctrl2, uint8_t ctrl5) {
  imuWrite(REG_CTRL7, 0x00);
  delay(2);
  imuWrite(REG_CTRL1, 0x40);
  imuWrite(REG_CTRL2, ctrl2);
  if (ctrl5 != 0xFF) imuWrite(REG_CTRL5, ctrl5);
  imuWrite(REG_CTRL7, 0x01);
}

// Debug (wom): QMI8658 wake-on-motion. Arm it, then for `secs` print the status registers whenever
// they are non-zero, plus one accel reading a second (does data keep coming in WoM mode?).
static bool imuCmd(uint8_t cmd) {                  // CTRL9 command handshake
  imuWrite(0x0A, cmd);
  uint32_t t0 = millis();
  uint8_t st = 0;
  do { boardI2CRead(s_addr, 0x2D, &st, 1); } while (!(st & 0x80) && millis() - t0 < 100);
  bool ok = st & 0x80;
  imuWrite(0x0A, 0x00);                            // ack
  t0 = millis();
  do { boardI2CRead(s_addr, 0x2D, &st, 1); } while ((st & 0x80) && millis() - t0 < 100);
  return ok;
}
static bool womArm(uint8_t mg, uint8_t ctrl2) {
  imuWrite(REG_CTRL7, 0x00);
  delay(2);
  imuWrite(REG_CTRL1, 0x48);                       // auto-increment + INT1 enable
  imuWrite(REG_CTRL2, ctrl2);                      // 0x1D: +-4 g, low-power 21 Hz
  imuWrite(REG_CTRL5, 0x00);
  imuWrite(0x0B, mg);                              // CAL1_L: threshold, mg
  imuWrite(0x0C, 0x02);                            // CAL1_H: INT1, initial 0, blank 2 samples
  bool ok = imuCmd(0x08);                          // CTRL_CMD_WRITE_WOM_SETTING
  imuWrite(REG_CTRL7, 0x01);
  return ok;
}
// Bench (`womint`): does the expander's INT reach GPIO0? The schematic has R42, a 3-pad either-or
// footprint: TCA9554 INT -> 3.3 V (pull-up) or -> GPIO0 (the BOOT pin, a deep-sleep wake pin).
// Arm wake-on-motion (it toggles INT1 -> expander input 5 -> expander INT, held low until the
// expander's input register is read), then watch GPIO0. Don't press BOOT during the test.
void motionWomIntTest(int secs, void (*out)(const char*)) {
  char b[120];
  bool ok = womArm(30, 0x1D);
  delay(60);
  motionWomFired();
  uint8_t in = 0, last = 0;
  boardI2CRead(0x20, 0x00, &last, 1);              // reading the input register clears the expander INT
  snprintf(b, sizeof(b), "womint: armed %s, expander in 0x%02X, GPIO0 %d - knock / tap the board now (%d s)",
           ok ? "ok" : "NO ACK", last, digitalRead(0), secs);
  out(b);
  uint32_t t0 = millis(), lastRead = 0;
  int lows = 0, toggles = 0, flags = 0;
  bool wasLow = false;
  while (millis() - t0 < (uint32_t)secs * 1000) {
    bool low = digitalRead(0) == LOW;
    if (low && !wasLow) {
      lows++;
      snprintf(b, sizeof(b), "womint: GPIO0 went LOW at %lu ms", (unsigned long)(millis() - t0));
      out(b);
    }
    wasLow = low;
    if (millis() - lastRead >= 300) {                // also watch INT1 itself on the expander
      lastRead = millis();
      if (boardI2CRead(0x20, 0x00, &in, 1) && ((in ^ last) & 0x18)) {
        toggles++;
        snprintf(b, sizeof(b), "womint: expander in 0x%02X (IMU INT1 bit4 %d, INT2 bit3 %d)", in, (in >> 4) & 1, (in >> 3) & 1);
        out(b);
      }
      last = in;
      if (motionWomFired()) {                          // the IMU's own flag (what the sleep peek reads)
        flags++;
        snprintf(b, sizeof(b), "womint: IMU knock flag at %lu ms", (unsigned long)(millis() - t0));
        out(b);
      }
    }
    delay(2);
  }
  snprintf(b, sizeof(b), "womint: done - knock flags %d, IMU INT pin toggles %d, GPIO0 lows %d -> %s", flags, toggles, lows,
           !flags && !toggles ? "no knock seen (knock harder?)" :
           !toggles ? "knocks seen but the INT pin never moved (pin config?)" :
           lows ? "INT REACHES GPIO0: wake-on-knock possible" : "INT does NOT reach GPIO0 (R42 = pull-up)");
  out(b);
  motionInit();                                    // back to normal readings
}

bool motionArmWom(uint8_t mg) {
  if (!g_motion.ok) return false;
  bool ok = womArm(mg, 0x1D);
  delay(60);
  motionWomFired();                                // drop anything the arming itself flagged
  return ok;
}
bool motionWomFired() {
  uint8_t st = 0;
  return boardI2CRead(s_addr, 0x2F, &st, 1) && (st & 0x04);
}
void motionWomTest(uint8_t mg, uint8_t ctrl2, int secs) {
  bool ok = womArm(mg, ctrl2);
  Serial.printf("wom: armed %u mg, CTRL2 0x%02X, cmd %s. Knock now.\n", mg, ctrl2, ok ? "ok" : "NO ACK");
  delay(200);
  uint8_t s[3];
  boardI2CRead(s_addr, 0x2D, s, 3);                // clear anything from arming
  uint32_t t0 = millis(), lastA = 0;
  int hits = 0;
  while (millis() - t0 < (uint32_t)secs * 1000) {
    if (boardI2CRead(s_addr, 0x2D, s, 3) && (s[0] | s[2])) {
      hits++;
      Serial.printf("wom: %5lu ms  STATUSINT %02X  STATUS0 %02X  STATUS1 %02X\n", millis() - t0, s[0], s[1], s[2]);
    }
    if (millis() - lastA > 1000) {
      lastA = millis();
      float x, y, z;
      if (motionReadAccel(x, y, z)) Serial.printf("wom: accel %.3f %.3f %.3f\n", x, y, z);
    }
    delay(50);
  }
  Serial.printf("wom: done, %d flagged reads\n", hits);
  motionInit();
}

// Debug (womcheck): the sleep peek on the bench. Arm wake-on-motion, wait `secs` (move it now),
// then read 16 samples the way quickWakeCheck does. reset = soft-reset the IMU first.
void motionWomCheck(bool reset, int secs) {
  float ref[3] = {0, 0, 0};
  imuConfigure(true); delay(300);
  for (int i = 0; i < 10; i++) { float x, y, z; motionReadAccel(x, y, z); ref[0] += x / 10; ref[1] += y / 10; ref[2] += z / 10; delay(10); }
  bool armed = motionArmWom(30);
  Serial.printf("womcheck: armed %d, ref %.3f %.3f %.3f - move / tilt it now (%d s)\n", armed, ref[0], ref[1], ref[2], secs);
  delay(secs * 1000);
  bool fired = motionWomFired();
  if (reset) { imuWrite(REG_RESET, 0xB0); delay(15); }
  imuConfigure(true);
  delay(120);
  float sum[3] = {0, 0, 0}, m[16];
  for (int i = 0; i < 16; i++) {
    float x, y, z; motionReadAccel(x, y, z);
    sum[0] += x; sum[1] += y; sum[2] += z; m[i] = sqrtf(x * x + y * y + z * z);
    if (i < 4 || i == 15) Serial.printf("  sample %2d: %.4f %.4f %.4f\n", i, x, y, z);
    delay(36);
  }
  float mean = 0, var = 0, d = 0;
  for (int i = 0; i < 16; i++) mean += m[i] / 16;
  for (int i = 0; i < 16; i++) var += (m[i] - mean) * (m[i] - mean) / 16;
  for (int k = 0; k < 3; k++) d = fmaxf(d, fabsf(sum[k] / 16 - ref[k]));
  Serial.printf("womcheck: %s, knock flag %d, moved %.3f g, vib %.4f g\n", reset ? "with reset" : "no reset", fired, d, sqrtf(var));
  motionInit();
}

bool motionProbeOnly() {
  g_motion.ok = imuProbe();
  return g_motion.ok;
}

bool motionReadTemp(float& c) {
  uint8_t b[2];
  if (!boardI2CRead(s_addr, REG_TEMP_L, b, 2)) return false;
  c = (int16_t)(b[0] | b[1] << 8) / 256.0f;
  return true;
}

// ------------------------------------------------------- car frame -------
static float s_up[3] = {0, 0, 1}, s_fwd[3] = {1, 0, 0}, s_left[3] = {0, 1, 0};

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void norm3(float a[3]) {
  float n = sqrtf(dot3(a, a));
  if (n < 1e-6f) return;
  a[0] /= n; a[1] /= n; a[2] /= n;
}

static void buildFrame() {
  s_up[0] = g_set.upX; s_up[1] = g_set.upY; s_up[2] = g_set.upZ;
  norm3(s_up);
  // The screen faces the driver, so "forward" is the chip Z axis projected onto
  // the horizontal plane (sign fixed by fwdSign). If the board lies flat, use Y.
  // On the 1.85" board the QMI8658's +Z points out of the BACK (away from the driver):
  // verified on the bench, calibrated upright then laid on its back = braking.
  float z[3] = {0, 0, 1};
  float d = dot3(z, s_up);
  float f[3] = {z[0] - d * s_up[0], z[1] - d * s_up[1], z[2] - d * s_up[2]};
  if (sqrtf(dot3(f, f)) < 0.3f) {
    float y[3] = {0, 1, 0};
    d = dot3(y, s_up);
    f[0] = y[0] - d * s_up[0]; f[1] = y[1] - d * s_up[1]; f[2] = y[2] - d * s_up[2];
  }
  norm3(f);
  for (int i = 0; i < 3; i++) s_fwd[i] = f[i] * g_set.fwdSign;
  // left = up x fwd
  s_left[0] = s_up[1] * s_fwd[2] - s_up[2] * s_fwd[1];
  s_left[1] = s_up[2] * s_fwd[0] - s_up[0] * s_fwd[2];
  s_left[2] = s_up[0] * s_fwd[1] - s_up[1] * s_fwd[0];
}

bool motionInit() {
  if (!imuProbe()) { g_motion.ok = false; return false; }
  imuWrite(REG_RESET, 0xB0);
  delay(15);
  imuConfigure(false);
  buildFrame();
  g_motion.ok = true;
  return true;
}

void motionFlipForward() {
  g_set.fwdSign = -g_set.fwdSign;
  settingsSave();
  buildFrame();
}

// ---------------------------------------------------------- calibration --
static uint32_t s_calStart = 0;
static float s_calSum[3], s_calSq = 0, s_calSumMag = 0, s_calGyr[3];
static int s_calN = 0;
// Gyro zero offset (deg/s). The QMI8658 reads a few deg/s while perfectly still, which looked like
// a constant slow turn ("driving") and kept the drag tree stuck at PRE-STAGE. Measured during
// calibration and kept up to date whenever the board sits still (it drifts with temperature).
static RTC_DATA_ATTR float s_gBias[3] = {0, 0, 0};

void motionStartCalibration() {
  g_motion.calibrating = true;
  g_motion.calibProgress = 0;
  s_calStart = millis();
  s_calSum[0] = s_calSum[1] = s_calSum[2] = 0;
  s_calSq = s_calSumMag = 0;
  s_calGyr[0] = s_calGyr[1] = s_calGyr[2] = 0;
  s_calN = 0;
}

// Upside down (roof mount)? Gravity's part in the screen plane (chip X/Y) says which way the top
// of the screen points. Mounted nearly flat (screen facing the roof or the floor) it can't tell:
// keep what's set (menu FLIP SCREEN is the override for that).
static void screenFlipFromGravity() {
  float ux = g_set.upX, uy = g_set.upY, uz = g_set.upZ;
  float mag = sqrtf(ux * ux + uy * uy + uz * uz), inPlane = sqrtf(ux * ux + uy * uy);
  if (mag < 0.5f || inPlane < 0.34f * mag) {   // tilted less than ~20 deg from flat
    Serial.printf("calibrate: up %.2f %.2f %.2f, lying flat: screen flip stays %d\n", ux, uy, uz, g_set.screenFlip);
    return;
  }
  int flip = (ux * SCREEN_UP_X + uy * SCREEN_UP_Y) < 0 ? 1 : 0;
  Serial.printf("calibrate: up %.2f %.2f %.2f -> screen %s\n", ux, uy, uz, flip ? "upside down (flipped)" : "upright");
  if (flip != g_set.screenFlip) { g_set.screenFlip = flip; displaySetFlip(flip); }
}

static void calibStep(uint32_t now, const float a[3], const float g[3]) {
  float mag = sqrtf(dot3(a, a));
  s_calSum[0] += a[0]; s_calSum[1] += a[1]; s_calSum[2] += a[2];
  for (int i = 0; i < 3; i++) s_calGyr[i] += g[i];
  s_calSumMag += mag; s_calSq += mag * mag; s_calN++;
  float elapsed = (now - s_calStart) / 2000.0f;
  g_motion.calibProgress = fminf(1.0f, elapsed);
  brainEvent(Expr::Calibrate, 400);

  if (s_calN > 20) {  // restart if it's being moved around
    float mean = s_calSumMag / s_calN;
    float var = s_calSq / s_calN - mean * mean;
    if (var > 0.0004f) { motionStartCalibration(); return; }
  }
  if (elapsed >= 1.0f && s_calN > 50) {
    g_set.upX = s_calSum[0] / s_calN; g_set.upY = s_calSum[1] / s_calN; g_set.upZ = s_calSum[2] / s_calN;
    for (int i = 0; i < 3; i++) s_gBias[i] = s_calGyr[i] / s_calN;
    Serial.printf("calibrate: gyro offset %.2f %.2f %.2f deg/s\n", s_gBias[0], s_gBias[1], s_gBias[2]);
    screenFlipFromGravity();
    g_set.calibrated = true;
    settingsSave();
    buildFrame();
    g_motion.calibrating = false;
    brainEvent(Expr::Happy, 1200);
  }
}

// ------------------------------------------------------------ detection --
static uint32_t s_lastSample = 0;
static float s_lon = 0, s_lat = 0, s_vertSlow = 0, s_yaw = 0;
static uint32_t s_brakeMs = 0, s_accelMs = 0, s_turnLMs = 0, s_turnRMs = 0;
static bool s_turnLatched = false;
static uint32_t s_turnTimes[8];
static int s_turnIdx = 0;
static float s_magMean = 1, s_magVar = 0;
static float s_tempF = 25;
static uint32_t s_lastBump = 0;
static float s_vibSq = 0, s_idleVib = 0;
static float s_gyroLP = 0, s_vert3 = 0, s_roadSq = 0;
// "Really stopped": steady cruising has no accel / turn / brake either, so stoppedSecs counts up
// on a straight road too (the IMU can't see speed). That made road shake look like revs and a
// smooth idle look like the engine stopping (owner's drive, Nov 19 2026). On the road the body
// still rolls / pitches and bounces; at a light it doesn't. Thresholds are first guesses:
// `drivelog` records these numbers on a drive to tune them.
// The limits live in g_set.quietGyro / quietRoad (defaults 0.6 dps wobble / 12 mg); recomputeQuiet()
// sets them from what ENGINE LEARN (idle) and "learn the road" (cruising) measured on this car.

// ---- ENGINE LEARN (menu): parked, engine idling in P -> 20 s; then engine off -> 10 s ----
enum class Learn : uint8_t { Off, Idle, WaitOff, EngOff, Road };
static Learn s_learn = Learn::Off;
static const char* s_learnTitle = "ENGINE LEARN";   // card title: ENGINE LEARN / LEARN THE ROAD
static uint32_t s_learnAt = 0, s_lowSince = 0;
static uint32_t s_lN = 0;
static float s_lIdleVib = 0, s_lIdleRoad = 0, s_lIdleGyro = 0;
static volatile bool s_lTap = false;
static const uint32_t L_IDLE_MS = 20000, L_OFF_MS = 10000, L_SETTLE_MS = 2000, L_WAIT_MS = 90000;
static const float L_FLOOR_DEFAULT = 0.0006f;   // a parked board's own reading, if the off step is skipped
// "learn the road" (voice, while cruising): 20 s of samples at ~10 Hz; the quiet end (20th percentile)
// is what counts - the threshold has to stay below the smoothest moments of real driving
static const uint32_t L_ROAD_MS = 20000;
static const int ROAD_N = 220;
static float s_rG[ROAD_N], s_rR[ROAD_N];
// Engine not felt (ENGINE LEARN set engineFelt = 0): the idle reads like engine off, so at a stop
// the cabin has to go quiet too: 12 dB under its level 2 s into the stop. Civic log, Nov 23 2026:
// real engine-offs fell 17+ dB (-22 -> -45..-58 with the defroster on); a running engine at a light
// fell 9 dB at most. Or long fully still (75 s at sleepMin 2): the mic isn't on (MUSIC BOB off), or
// the car was already quiet when the stop began. Called every update (keeps the stop's reference).
static float s_stopDb = 0;
static bool s_stopRef = false;
static const char* s_offWhy = "";
static bool cabinQuiet() {
  if (g_set.engineFelt) return true;
  float lvl, beat, bpm;
  musicSample(lvl, beat, bpm);
  bool live = musicLive();
  if (g_motion.quietSecs < 1.0f) s_stopRef = false;
  else if (!s_stopRef && g_motion.quietSecs >= 2.0f && live) { s_stopDb = lvl; s_stopRef = true; }
  if (live && s_stopRef && lvl < s_stopDb - 12.0f) { s_offWhy = "off: cabin went quiet"; return true; }
  // Fallback: the summary comes 8 s (off) + 20 s (summary wait) later, and sleep at sleepMin still
  // doesn't wait for it, so stay ~45 s under the sleep time (sleepMin 2 -> 75 s), at most 100 s.
  float fallback = constrain(g_set.sleepMin * 60.0f - 45.0f, 30.0f, 100.0f);
  if (g_motion.quietSecs > fallback) { s_offWhy = "off: long still"; return true; }
  return false;
}
static float s_lV[ROAD_N];   // ENGINE LEARN: vibration samples (with s_rR / s_rG for road / wobble), ~10 Hz
static float pctl(float* v, int n, float p) {   // sorts v in place
  for (int i = 1; i < n; i++) { float t = v[i]; int j = i - 1; while (j >= 0 && v[j] > t) { v[j + 1] = v[j]; j--; } v[j + 1] = t; }
  return v[constrain((int)(p * (n - 1) + 0.5f), 0, n - 1)];
}
static int s_rN = 0;
static uint32_t s_rLast = 0;
// Sound (for the music detector), sampled with the learns every 500 ms while the analysis runs:
// level / beat / tempo.
static const int SND_N = 64;
static float s_sL[SND_N], s_sB[SND_N], s_sE[SND_N];
static int s_sN = 0;
static uint32_t s_sLast = 0;

static void soundReset() { s_sN = 0; s_sLast = 0; }
static void soundSample(uint32_t now) {
  // (no "music heard" check: the V8 idling is what fooled the detector, so it would refuse the very
  // noise it has to learn. The card says MUSIC OFF; learning with music on = just run it again.)
  if (!musicLive() || now - s_sLast < 500 || s_sN >= SND_N) return;
  s_sLast = now;
  musicSample(s_sL[s_sN], s_sB[s_sN], s_sE[s_sN]);
  if (s_sL[s_sN] > -99) s_sN++;
}
static float pctOf(float* v, int n, float q) {   // sorts v in place
  for (int i = 1; i < n; i++) { float t = v[i]; int j = i - 1; while (j >= 0 && v[j] > t) { v[j + 1] = v[j]; j--; } v[j + 1] = t; }
  return v[(int)(n * q)];
}
// "" = saved; otherwise why the sound wasn't learned (shown on the card)
static const char* soundIdleDone() {
  if (s_sN < 12) return g_set.bobOn ? "MIC NOT READ - SOUND NOT LEARNED" : "MUSIC BOB OFF - SOUND NOT LEARNED";
  // the engine's tempo: the estimate most others agree with (within 6 %), if a third of them do
  float bestBpm = 0; int bestN = 0;
  for (int i = 0; i < s_sN; i++) {
    int k = 0;
    for (int j = 0; j < s_sN; j++) if (fabsf(s_sE[j] - s_sE[i]) < s_sE[i] * 0.06f) k++;
    if (k > bestN) { bestN = k; bestBpm = s_sE[i]; }
  }
  int n = s_sN;
  g_set.idleNoiseDb = pctOf(s_sL, n, 0.5f);
  g_set.idleBeat = pctOf(s_sB, n, 0.5f);
  g_set.idleBpm = bestN * 3 >= n ? bestBpm : 0;
  Serial.printf("engine learn: sound %.1f dBFS, beat %.2f, tempo %.0f BPM (%d of %d agree)%s\n", g_set.idleNoiseDb,
                g_set.idleBeat, bestBpm, bestN, n, g_set.idleBpm > 0 ? "" : " - no steady rhythm");
  return "";
}
static const char* soundRoadDone() {
  if (s_sN < 12) return g_set.bobOn ? "MIC NOT READ - SOUND NOT LEARNED" : "MUSIC BOB OFF - SOUND NOT LEARNED";
  g_set.cruiseNoiseDb = pctOf(s_sL, s_sN, 0.3f);   // the quieter end: road noise rises with speed
  Serial.printf("road learn: sound %.1f dBFS (%d readings)\n", g_set.cruiseNoiseDb, s_sN);
  return "";
}
static const char* s_sNote = "";   // the sound result, for the done card

static uint32_t s_engOnMs = 0, s_engOffMs = 0, s_lastRev = 0;
static bool s_inRev = false;

static void recordTurn(uint32_t now) {
  s_turnTimes[s_turnIdx++ % 8] = now;
  int n = 0;
  for (uint32_t tt : s_turnTimes) if (tt && now - tt < 30000) n++;
  if (n >= 5) {
    Serial.println("motion: dizzy (5 turns in 30 s)");
    brainEvent(Expr::Dizzy, 2500);
    memset(s_turnTimes, 0, sizeof(s_turnTimes));
  }
}

float g_dbgMagVar = 0, g_dbgGyro = 0;
static void engineLearnStep(uint32_t now, float vib, bool selfNoise);

void motionUpdate(uint32_t now) {
  if (!g_motion.ok) return;
  if (now - s_lastSample < 9) return;  // ~110 Hz max
  float dt = (now - s_lastSample) / 1000.0f;
  if (dt > 0.1f) dt = 0.1f;
  s_lastSample = now;

  float a[3], g[3], temp;
  if (!readAll(a, g, temp)) return;
  s_tempF += (temp - s_tempF) * 0.01f;
  g_motion.tempC = s_tempF;

  if (g_motion.calibrating) { calibStep(now, a, g); return; }
  for (int i = 0; i < 3; i++) g[i] -= s_gBias[i];

  // car frame
  float lonRaw = dot3(a, s_fwd), latRaw = dot3(a, s_left), vertRaw = dot3(a, s_up) - 1.0f;
  float yawRaw = dot3(g, s_up);
  float k = 1 - expf(-dt * 2 * PI * 3.0f);      // ~3 Hz low-pass
  s_lon += (lonRaw - s_lon) * k;
  s_lat += (latRaw - s_lat) * k;
  s_yaw += (yawRaw - s_yaw) * k;
  float ks = 1 - expf(-dt * 2 * PI * 0.5f);     // slow, for bump high-pass
  s_vertSlow += (vertRaw - s_vertSlow) * ks;
  float bumpG = fabsf(vertRaw - s_vertSlow);

  g_motion.lon = s_lon; g_motion.lat = s_lat; g_motion.yaw = s_yaw; g_motion.vert = vertRaw - s_vertSlow;
  // road: vertical body motion between ~0.5 and ~3 Hz (engine shake is tens of Hz)
  s_vert3 += (vertRaw - s_vert3) * k;
  float band = s_vert3 - s_vertSlow;
  s_roadSq += (band * band - s_roadSq) * (1 - expf(-dt / 1.0f));
  g_motion.road = sqrtf(s_roadSq);

  // stillness: |a| variance + gyro magnitude
  float mag = sqrtf(dot3(a, a));
  float km = 1 - expf(-dt * 2 * PI * 0.5f);
  s_magMean += (mag - s_magMean) * km;
  s_magVar += ((mag - s_magMean) * (mag - s_magMean) - s_magVar) * km;
  float gyroMag = sqrtf(dot3(g, g));
  // rotation wobble, not rotation: |gyro - its own ~5 s average|. A leftover offset (7.6 dps on the
  // bench, Nov 19 2026: the offset only re-learns when the mount sits level) or a long steady curve
  // drops out; the body rolling / pitching on the road doesn't.
  static float s_gAvg[3] = {0, 0, 0};
  static bool s_gSeeded = false;
  if (!s_gSeeded) { for (int i = 0; i < 3; i++) s_gAvg[i] = g[i]; s_gSeeded = true; }   // else ~15 s of fake wobble after boot
  float kg = 1 - expf(-dt / 5.0f), wob2 = 0;
  for (int i = 0; i < 3; i++) { s_gAvg[i] += (g[i] - s_gAvg[i]) * kg; float d = g[i] - s_gAvg[i]; wob2 += d * d; }
  s_gyroLP += (sqrtf(wob2) - s_gyroLP) * (1 - expf(-dt / 1.0f));
  g_motion.gyroLP = s_gyroLP;
  bool quiet = s_gyroLP < g_set.quietGyro && g_motion.road < g_set.quietRoad;
  bool still = s_magVar < 0.00015f && gyroMag < 1.5f;
  g_dbgMagVar = s_magVar; g_dbgGyro = gyroMag;           // for `awake`
  if (!still && g_motion.stillSecs > 20)
    Serial.printf("still: reset after %.0f s (accel var %.6f, gyro %.2f dps, |a| %.3f)\n", g_motion.stillSecs, s_magVar, gyroMag, mag);
  g_motion.stillSecs = still ? g_motion.stillSecs + dt : 0;
  // Keep the gyro offset fresh: sitting still (steady accel, no sideways g, small rotation)
  // -> slowly pull the offset toward what the gyro reads now (~10 s time constant).
  if (s_magVar < 0.00015f && fabsf(s_lat) < 0.05f && gyroMag < 8.0f) {
    float kb = dt / 10.0f;
    for (int i = 0; i < 3; i++) s_gBias[i] += g[i] * kb;
  }

  bool driving = fabsf(s_lon) > 0.08f || fabsf(s_lat) > 0.08f || fabsf(s_yaw) > 5;
  if (driving) { g_motion.hadDriven = true; g_motion.stoppedSecs = 0; }
  else g_motion.stoppedSecs += dt;
  g_motion.quietSecs = quiet && !driving ? g_motion.quietSecs + dt : 0;
  if (g_motion.stoppedSecs > 900) g_motion.hadDriven = false;

  uint32_t ms = (uint32_t)(dt * 1000);

  // braking
  if (s_lon < -g_set.brake) {
    s_brakeMs += ms;
    if (s_brakeMs >= 150) {
      if (s_brakeMs - ms < 150) { tripHard(Hard::Brake); Serial.printf("motion: braking %.2f g\n", -s_lon); }
      brainEvent(Expr::Braking, 900);
    }
  } else s_brakeMs = 0;

  // accelerating / V8 launch
  if (s_lon > g_set.accel) {
    s_accelMs += ms;
    if (s_accelMs >= 250) {
      if (s_accelMs - ms < 250) {
        tripHard(Hard::Accel);
        Serial.printf("motion: %s %.2f g\n", g_set.v8On && s_lon > g_set.v8 ? "V8 launch" : "accel", s_lon);
      }
      if (g_set.v8On && s_lon > g_set.v8) brainEvent(Expr::V8, 1800);
      else brainEvent(Expr::Accel, 900);
    }
  } else s_accelMs = 0;

  // turns (yaw rate)
  if (s_yaw > g_set.turn) {
    s_turnLMs += ms;
    if (s_turnLMs >= 200) {
      brainEvent(Expr::TurnLeft, 600);
      if (!s_turnLatched) { s_turnLatched = true; Serial.printf("motion: turn left %.0f deg/s\n", s_yaw); recordTurn(now); tripHard(Hard::Turn); }
    }
  } else s_turnLMs = 0;
  if (s_yaw < -g_set.turn) {
    s_turnRMs += ms;
    if (s_turnRMs >= 200) {
      brainEvent(Expr::TurnRight, 600);
      if (!s_turnLatched) { s_turnLatched = true; Serial.printf("motion: turn right %.0f deg/s\n", -s_yaw); recordTurn(now); tripHard(Hard::Turn); }
    }
  } else s_turnRMs = 0;
  if (fabsf(s_yaw) < g_set.turn * 0.6f) s_turnLatched = false;

  // bumps
  if (bumpG > g_set.bump) {
    if (now - s_lastBump > 500) { tripHard(Hard::Bump); Serial.printf("motion: bump %.2f g\n", bumpG); }
    s_lastBump = now;
    brainEvent(Expr::Bump, 700);
  }

  // ---- engine vibration (V8 idle shows up as small fast |a| wiggles) ----
  float hp = mag - s_magMean;
  float kv = 1 - expf(-dt / 0.3f);
  s_vibSq += (hp * hp - s_vibSq) * kv;
  float vib = sqrtf(s_vibSq);
  g_motion.vib = vib;

  // The speaker shakes the board (V8 rumble, clips): hold the engine counters while it plays
  // and briefly after, or Otto "hears" its own sound as the engine starting / stopping.
  static uint32_t s_audioAt = 0;
  if (audioBusy()) s_audioAt = now;
  bool selfNoise = s_audioAt && now - s_audioAt < 400;
  int obdEng = obdEngineState();                 // OBD knows for sure: no vibration guessing
  bool cabinOff = cabinQuiet();
  if (obdEng >= 0) {
    if (obdEng) { s_engOnMs += ms * 5; s_engOffMs = 0; } else { s_engOffMs += ms * 3; s_engOnMs = 0; }
  }
  else if (selfNoise) { /* hold */ }
  else if (vib > g_set.engineVib) { s_engOnMs += ms; s_engOffMs = 0; }
  // Off = vibration down near the sensor's own floor (a parked car reads ~0.001 g), not just under
  // the "on" level: a smooth V8 idling in Drive at a light sat below the old 0.6 x engineVib and
  // ended the drive at every stop. Also needs a quiet body for 8 s (not cruising).
  else if (vib < (g_set.engOffVib > 0 ? g_set.engOffVib : g_set.engineVib * 0.3f) && g_motion.quietSecs > 1.0f && cabinOff) {
    s_engOffMs += ms; s_engOnMs = 0;
  }
  else if (!g_set.engineFelt) s_engOffMs = 0;   // not felt: the quiet cabin has to hold 8 s in a row
  if (!g_motion.engineOn && s_engOnMs > 1500) { g_motion.engineOn = true; g_motion.engineStarted = true; Serial.println("motion: engine on"); drivelogEvent("engine on"); }
  if (g_motion.engineOn && s_engOffMs > 8000) {
    g_motion.engineOn = false; g_motion.engineStopped = true;
    Serial.printf("motion: engine off%s%s\n", *s_offWhy ? " - " : "", s_offWhy);
    drivelogEvent("engine off");
    if (!g_set.engineFelt && *s_offWhy) drivelogEvent(s_offWhy);
  }

  // ---- revs: really stopped with the engine running, vibration jumps well above idle ----
  // quietSecs: the body was still for 3 s before the jump (a rev's own torque roll comes after)
  bool parked = g_motion.engineOn && g_motion.stoppedSecs > 2.0f && (s_inRev || g_motion.quietSecs > 3.0f);
  if (parked && obdRpmFresh()) { /* obd.cpp does revs from the real rpm */ }
  else if (parked && g_set.engineFelt) {          // not felt: no idle to see a rev against
    if (!s_inRev) {
      if (s_idleVib <= 0 && g_set.idleVib > 0) s_idleVib = g_set.idleVib;   // start from the learned idle
      s_idleVib += (vib - s_idleVib) * (1 - expf(-dt / 5.0f));            // follow today's idle
    }
    float base = fmaxf(s_idleVib, g_set.idleVib > 0 ? g_set.idleVib : g_set.engineVib);
    if (!s_inRev && vib > base * g_set.revRatio && now - s_lastRev > 400) {
      s_inRev = true;
      s_lastRev = now;
      if (g_set.revOn) {
        float intensity = constrain(vib / base, 1.0f, 6.0f);
        brainSpinImpulse(350 * intensity);
        brainEvent(Expr::Rev, 1100);
        drivelogEvent("rev");
      }
    }
    if (s_inRev && vib < base * 1.3f) s_inRev = false;
  } else {
    s_inRev = false;
    if (!g_motion.engineOn) s_idleVib = 0;
  }

  engineLearnStep(now, vib, selfNoise);
  recorderSample(now, -s_lon, s_lon, fabsf(s_lat), fabsf(s_yaw), bumpG);
}

// ---------------------------------------------------------- ENGINE LEARN --
bool motionEngineLearning() { return s_learn != Learn::Off; }
void motionEngineLearnTap() { s_lTap = true; }

void motionStartEngineLearn() {
  if (!g_motion.ok || g_motion.calibrating) return;
  s_learnTitle = "ENGINE LEARN";
  s_learn = Learn::Idle; s_learnAt = millis(); s_lN = 0; s_rLast = 0; s_lTap = false;
  soundReset(); s_sNote = "";
  Serial.println("engine learn: measuring idle (engine running, in P)");
  audioChirp(700, 1000, 120);
}

// quietGyro / quietRoad from what's been learned. Both known: the geometric middle between idle and
// cruising (if cruising is clearly the shakier one). Idle only: 2.5 x idle. Neither: defaults.
static void recomputeQuiet() {
  // cruising only counts against a measured idle (a default idle is a guess; bench, Nov 19 2026)
  bool idleKnown = g_set.idleGyro > 0 && g_set.idleRoad > 0;
  if (!idleKnown) return;
  float iG = g_set.idleGyro, iR = g_set.idleRoad;
  if (g_set.cruiseGyro > iG * 1.5f) g_set.quietGyro = sqrtf(iG * g_set.cruiseGyro);
  else g_set.quietGyro = iG * 2.5f;
  if (g_set.cruiseRoad > iR * 1.5f) g_set.quietRoad = sqrtf(iR * g_set.cruiseRoad);
  else g_set.quietRoad = iR * 2.5f;
  g_set.quietGyro = constrain(g_set.quietGyro, 0.3f, 2.5f);
  g_set.quietRoad = constrain(g_set.quietRoad, 0.004f, 0.03f);
}

static void learnCard(const char* big, const char* sub, float ring) {
  brainSetAnswer(s_learnTitle, big, sub);
  brainSetRing(ring);
  brainEventPrio(Expr::EngineLearn, 700, 88);
}

static void learnDone(float floorV, bool measured) {
  s_learn = Learn::Off;
  char sub[40];
  // Idle and off this close, with the engine really turned off: the IMU can't feel this engine
  // (the Civic in its mount: 0.10 mg either way, Nov 23 2026). Vibration still tells driving from
  // stopped, so keep that, and let the cabin's sound decide engine off at a stop (cabinQuiet()).
  if (measured && (s_lIdleVib < floorV * 2.5f || s_lIdleVib < 0.0008f)) {
    float hi = fmaxf(s_lIdleVib, floorV);
    g_set.engineFelt = 0;
    g_set.idleVib = 0;                               // no idle to rev above: revs off (unless OBD)
    g_set.engineVib = fmaxf(0.001f, hi * 6);         // driving reads 10+ mg: "on" stays easy
    g_set.engOffVib = fminf(hi * 2.5f, g_set.engineVib * 0.8f);
    g_set.idleGyro = s_lIdleGyro; g_set.idleRoad = s_lIdleRoad;
    recomputeQuiet();
    settingsSave();
    Serial.printf("engine learn: idle %.4f g vs off %.4f g - can't feel the engine; stops listen instead "
                  "(on > %.4f, off < %.4f + quiet cabin)\n", s_lIdleVib, floorV, g_set.engineVib, g_set.engOffVib);
    char ev[64];
    snprintf(ev, sizeof(ev), "learn idle %.4f off %.4f not felt", s_lIdleVib, floorV);
    drivelogEvent(ev);
    brainSetAnswer("ENGINE LEARN", "EAR", "CAN'T FEEL IT - STOPS LISTEN");
    brainEventPrio(Expr::EngineLearn, 5000, 88);
    audioChirp(900, 1200, 150);
    return;
  }
  // Can't tell idle from off when they're this close (off step skipped): keep what he had.
  if (s_lIdleVib < floorV * 2.5f || s_lIdleVib < 0.0008f) {
    Serial.printf("engine learn: idle %.4f g vs off %.4f g - too close to tell apart, nothing saved\n", s_lIdleVib, floorV);
    snprintf(sub, sizeof(sub), "IDLE %.1f MG - OFF %.1f MG", s_lIdleVib * 1000, floorV * 1000);
    brainSetAnswer("ENGINE LEARN", "?", sub);
    brainEventPrio(Expr::EngineLearn, 5000, 88);
    audioChirp(600, 400, 250);
    if (!*s_sNote) settingsSave();             // the sound part stands on its own
    return;
  }
  // geometric steps between the two levels: "on" halfway (in ratio) from off to idle, "off" lower still
  g_set.engineFelt = 1;
  g_set.idleVib = s_lIdleVib;
  g_set.engineVib = sqrtf(floorV * s_lIdleVib);
  g_set.engOffVib = sqrtf(floorV * g_set.engineVib);
  // really stopped = this car's idle stillness with room to spare (or halfway to cruising, if known)
  g_set.idleGyro = s_lIdleGyro; g_set.idleRoad = s_lIdleRoad;
  recomputeQuiet();
  settingsSave();
  Serial.printf("engine learn: idle %.4f g, off %.4f g%s -> on > %.4f, off < %.4f; quiet gyro %.2f dps, road %.4f g\n",
                s_lIdleVib, floorV, measured ? "" : " (skipped: default)", g_set.engineVib, g_set.engOffVib,
                g_set.quietGyro, g_set.quietRoad);
  char ev[64];
  snprintf(ev, sizeof(ev), "learn idle %.4f off %.4f", s_lIdleVib, floorV);
  drivelogEvent(ev);
  snprintf(sub, sizeof(sub), "IDLE %.1f MG - OFF %.1f MG", s_lIdleVib * 1000, floorV * 1000);
  if (*s_sNote) snprintf(sub, sizeof(sub), "%s", s_sNote);   // vibration fine, but say why no sound
  brainSetAnswer("ENGINE LEARNED", "OK", sub);
  brainEventPrio(Expr::EngineLearn, 5000, 88);
  audioChirp(900, 1400, 150);
}

void motionStartRoadLearn() {
  if (!g_motion.ok || g_motion.calibrating) return;
  s_learnTitle = "LEARN THE ROAD";
  s_learn = Learn::Road; s_learnAt = millis(); s_rN = 0; s_rLast = 0; s_lTap = false;
  soundReset(); s_sNote = "";
  Serial.println("road learn: measuring 20 s of steady driving");
  audioChirp(700, 1000, 120);
}

static float pct20(float* v, int n) {          // sorts v in place
  for (int i = 1; i < n; i++) { float t = v[i]; int j = i - 1; while (j >= 0 && v[j] > t) { v[j + 1] = v[j]; j--; } v[j + 1] = t; }
  return v[n / 5];
}

static void roadDone() {
  s_learn = Learn::Off;
  char sub[40];
  if (s_rN < 50) { brainSetAnswer(s_learnTitle, "?", "TOO FEW READINGS - TRY AGAIN"); brainEventPrio(Expr::EngineLearn, 4000, 88); return; }
  float cG = pct20(s_rG, s_rN), cR = pct20(s_rR, s_rN);
  bool idleKnown = g_set.idleGyro > 0 && g_set.idleRoad > 0;
  float iG = idleKnown ? g_set.idleGyro : 0.3f, iR = idleKnown ? g_set.idleRoad : 0.0048f;
  Serial.printf("road learn: cruising wobble %.2f dps, road %.4f g (idle %.2f / %.4f%s)\n", cG, cR, iG, iR,
                idleKnown ? "" : ", not learned: run ENGINE LEARN too");
  // nothing shakier than idle: he was probably stopped (or the road is glass-smooth) - keep the old values
  if (cG < iG * 1.5f && cR < iR * 1.5f) {
    Serial.println("road learn: no shakier than idle - nothing saved");
    brainSetAnswer(s_learnTitle, "?", "LOOKS STOPPED - TRY WHILE DRIVING");
    brainEventPrio(Expr::EngineLearn, 5000, 88);
    audioChirp(600, 400, 250);
    return;
  }
  g_set.cruiseGyro = cG; g_set.cruiseRoad = cR;
  recomputeQuiet();
  s_sNote = soundRoadDone();
  settingsSave();
  Serial.printf("road learn: quiet = wobble < %.2f dps, road < %.4f g\n", g_set.quietGyro, g_set.quietRoad);
  char ev[64];
  snprintf(ev, sizeof(ev), "road learn wobble %.2f road %.4f", cG, cR);
  drivelogEvent(ev);
  if (idleKnown) snprintf(sub, sizeof(sub), "ROAD %.1fx IDLE", fmaxf(cG / iG, cR / iR));
  else snprintf(sub, sizeof(sub), "NOW RUN ENGINE LEARN (PARKED)");   // saved; used once idle is known
  if (*s_sNote) snprintf(sub, sizeof(sub), "%s", s_sNote);
  brainSetAnswer("ROAD LEARNED", "OK", sub);
  brainEventPrio(Expr::EngineLearn, 4000, 88);
  audioChirp(900, 1400, 150);
}

static void engineLearnStep(uint32_t now, float vib, bool selfNoise) {
  if (s_learn == Learn::Off) return;
  uint32_t el = now - s_learnAt;
  bool tap = s_lTap; s_lTap = false;
  bool sample = el > L_SETTLE_MS && !selfNoise;
  switch (s_learn) {
    case Learn::Idle:
      if (tap) { s_learn = Learn::Off; Serial.println("engine learn: cancelled"); brainEvent(Expr::Idle, 100); return; }
      if (sample) {
        soundSample(now);
        if (now - s_rLast >= 90 && s_lN < (uint32_t)ROAD_N) {
          s_rLast = now; s_lV[s_lN] = vib; s_rR[s_lN] = g_motion.road; s_rG[s_lN] = g_motion.gyroLP; s_lN++;
        }
      }
      learnCard("IDLE", "IN P, MUSIC OFF - TAP: STOP", fminf(1.0f, el / (float)L_IDLE_MS));
      if (el >= L_IDLE_MS && s_lN) {
        // Medians: averaged, one knock (the tap that started it, ~9 mg) made the Civic's silent idle
        // look 18x engine off and the learn saved junk instead of refusing (drive log, Nov 23 2026).
        s_lIdleVib = pctl(s_lV, s_lN, 0.5f); s_lIdleRoad = pctl(s_rR, s_lN, 0.5f); s_lIdleGyro = pctl(s_rG, s_lN, 0.5f);
        s_sNote = soundIdleDone();                 // saved with the rest in learnDone (settingsSave)
        Serial.printf("engine learn: idle vib %.4f g, road %.4f g, gyro %.2f dps; now turn the engine off\n",
                      s_lIdleVib, s_lIdleRoad, s_lIdleGyro);
        s_learn = Learn::WaitOff; s_learnAt = now; s_lowSince = 0;
        audioChirp(1000, 700, 150);
      }
      break;
    case Learn::WaitOff:
      learnCard("OFF", "TURN THE ENGINE OFF - TAP: SKIP", 0);
      if (tap || el > L_WAIT_MS) { learnDone(L_FLOOR_DEFAULT, false); return; }
      if (vib < s_lIdleVib * 0.5f) { if (!s_lowSince) s_lowSince = now; }
      else s_lowSince = 0;
      if (s_lowSince && now - s_lowSince > 1500) {
        s_learn = Learn::EngOff; s_learnAt = now; s_lN = 0;
        Serial.println("engine learn: engine off, measuring the quiet");
      }
      break;
    case Learn::EngOff:
      if (tap) { learnDone(L_FLOOR_DEFAULT, false); return; }
      if (sample && now - s_rLast >= 90 && s_lN < (uint32_t)ROAD_N) { s_rLast = now; s_lV[s_lN++] = vib; }
      learnCard("QUIET", "MEASURING ENGINE OFF", fminf(1.0f, el / (float)L_OFF_MS));
      if (el >= L_OFF_MS && s_lN) learnDone(fmaxf(0.0002f, pctl(s_lV, s_lN, 0.5f)), true);
      break;
    case Learn::Road:
      if (tap) { s_learn = Learn::Off; Serial.println("road learn: cancelled"); brainEvent(Expr::Idle, 100); return; }
      if (el > L_SETTLE_MS && now - s_rLast >= 90 && s_rN < ROAD_N) {
        s_rLast = now; s_rG[s_rN] = g_motion.gyroLP; s_rR[s_rN] = g_motion.road; s_rN++;
      }
      if (el > L_SETTLE_MS && !selfNoise) soundSample(now);
      learnCard("DRIVE", "DRIVE STEADY, MUSIC OFF", fminf(1.0f, el / (float)(L_ROAD_MS + L_SETTLE_MS)));
      if (el >= L_ROAD_MS + L_SETTLE_MS) roadDone();
      break;
    default: break;
  }
}
