#include "motion.h"
#include "obd.h"
#include "board.h"
#include "brain.h"
#include "settings.h"
#include "recorder.h"
#include "trip.h"
#include "audio.h"
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

  // stillness: |a| variance + gyro magnitude
  float mag = sqrtf(dot3(a, a));
  float km = 1 - expf(-dt * 2 * PI * 0.5f);
  s_magMean += (mag - s_magMean) * km;
  s_magVar += ((mag - s_magMean) * (mag - s_magMean) - s_magVar) * km;
  float gyroMag = sqrtf(dot3(g, g));
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
  if (obdEng >= 0) {
    if (obdEng) { s_engOnMs += ms * 5; s_engOffMs = 0; } else { s_engOffMs += ms * 3; s_engOnMs = 0; }
  }
  else if (selfNoise) { /* hold */ }
  else if (vib > g_set.engineVib) { s_engOnMs += ms; s_engOffMs = 0; }
  else if (vib < g_set.engineVib * 0.6f) { s_engOffMs += ms; s_engOnMs = 0; }
  if (!g_motion.engineOn && s_engOnMs > 1500) { g_motion.engineOn = true; g_motion.engineStarted = true; Serial.println("motion: engine on"); }
  if (g_motion.engineOn && s_engOffMs > 4000) { g_motion.engineOn = false; g_motion.engineStopped = true; Serial.println("motion: engine off"); }

  // ---- revs: parked with the engine running, vibration jumps well above idle ----
  bool parked = g_motion.engineOn && g_motion.stoppedSecs > 2.0f;
  if (parked && obdRpmFresh()) { /* obd.cpp does revs from the real rpm */ }
  else if (parked) {
    if (!s_inRev) s_idleVib += (vib - s_idleVib) * (1 - expf(-dt / 5.0f));   // learn idle level
    float base = fmaxf(s_idleVib, g_set.engineVib);
    if (!s_inRev && vib > base * g_set.revRatio && now - s_lastRev > 400) {
      s_inRev = true;
      s_lastRev = now;
      if (g_set.revOn) {
        float intensity = constrain(vib / base, 1.0f, 6.0f);
        brainSpinImpulse(350 * intensity);
        brainEvent(Expr::Rev, 1100);
      }
    }
    if (s_inRev && vib < base * 1.3f) s_inRev = false;
  } else {
    s_inRev = false;
    if (!g_motion.engineOn) s_idleVib = 0;
  }

  recorderSample(now, -s_lon, s_lon, fabsf(s_lat), fabsf(s_yaw), bumpG);
}
