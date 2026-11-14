// OTTO - dash companion firmware (1997 Mercedes-Benz E420)
// Waveshare ESP32-S3-Touch-LCD-1.46B

#include <Arduino.h>
#include <LittleFS.h>
#include "esp_sleep.h"
#include "driver/rtc_io.h"

#include "config.h"
#include "settings.h"
#include "board.h"
#include "display.h"
#include "face.h"
#include "brain.h"
#include "motion.h"
#include "power.h"
#include "audio.h"
#include "mic.h"
#include "voice.h"
#include "music.h"
#include "extras.h"
#include "recorder.h"
#include "greetings.h"
#include "input.h"
#include "ble.h"
#include "commands.h"
#include "touch.h"
#include "menu.h"
#include "launch.h"
#include "trip.h"
#include "ota.h"
#include "holidays.h"
#include "obd.h"

// Survive deep sleep
RTC_DATA_ATTR static float s_sleepRef[3] = {0, 0, 0};
RTC_DATA_ATTR static bool s_haveRef = false;
RTC_DATA_ATTR static bool s_hotSleep = false;     // sleeping because it got too hot
RTC_DATA_ATTR static bool s_wokeByVib = false;    // woken by engine vibration
RTC_DATA_ATTR static bool s_womArmed = false;     // the IMU's wake-on-motion watched this sleep
RTC_DATA_ATTR static uint8_t s_womQuiet = 0;      // peeks skipped since the last full look
static const uint8_t WOM_MG = 30;                 // bench: 30 mg caught 4 of 4 light table knocks, 60 mg 2 of 4
// Sleep-peek diagnostics, printed on the next full wake ("sleep: ..." line)
RTC_DATA_ATTR static uint32_t s_peeks = 0;
RTC_DATA_ATTR static float s_peekD = 0, s_peekSd = 0;
RTC_DATA_ATTR static int s_peekGot = -1;
RTC_DATA_ATTR static float s_peekMean[3] = {0, 0, 0};

static volatile bool s_wantSleep = false, s_wantOff = false;
void requestSleep() { s_wantSleep = true; }
void requestPowerOff() { s_wantOff = true; }

static FaceState s_face, s_target;
static float s_crankAngle = 0;   // crankshaft angle: the piston's stroke follows it
static uint32_t s_lastLoop = 0, s_lastFrame = 0, s_lastTelem = 0, s_greetAt = 0;
static uint32_t s_lastLowBatt = 0, s_lastHot = 0, s_hotSince = 0, s_lastGreet = 0, s_bootDone = 0;
static bool s_wasCharging = false, s_wasFull = false;
static bool s_batPending = true;    // show the battery level once, a few seconds after each wake
static bool s_chargeSoonShown = false;
static char s_serialLine[240];
static size_t s_serialLen = 0;

// ----------------------------------------------------------- deep sleep ---
static void averageAccel(float out[3], int n) {
  out[0] = out[1] = out[2] = 0;
  int got = 0;
  for (int i = 0; i < n; i++) {
    float x, y, z;
    // skip impossible readings for a parked board (mode-switch junk reads +-4 g on every axis)
    if (motionReadAccel(x, y, z) && x * x + y * y + z * z < 4.0f) { out[0] += x; out[1] += y; out[2] += z; got++; }
    delay(10);
  }
  if (got) { out[0] /= got; out[1] /= got; out[2] /= got; }
}

static void enterDeepSleep(uint32_t timerSecs) {
  uint64_t mask = (1ULL << PIN_BTN_BOOT) | (1ULL << PIN_BTN_PWR);
  touchPrepareSleep();
  // After a 4 s timer peek the pin isn't set up yet (touchInit didn't run), so an
  // unconfigured read could look LOW and silently drop touch wake from every later sleep.
  pinMode(PIN_TP_INT, INPUT_PULLUP);
  delay(30);   // let any INT pulse finish before checking the line is idle
  // touch wake only if the touch interrupt line is idle (otherwise it would wake instantly)
  if (g_set.touchWake && digitalRead(PIN_TP_INT) == HIGH) {
    mask |= (1ULL << PIN_TP_INT);
    rtc_gpio_pullup_en((gpio_num_t)PIN_TP_INT);
  }
  boardHoldPinsForSleep();
  rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_BOOT);
  rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_PWR);
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  if (timerSecs) esp_sleep_enable_timer_wakeup((uint64_t)timerSecs * 1000000ULL);
  Serial.flush();
  esp_deep_sleep_start();
}

// Back to sleep from a peek (or at the end of goToSleep): with motion wake on, the IMU's
// wake-on-motion watches until the next peek, so a knock between peeks isn't missed.
static void peekSleep() {
  s_womArmed = g_set.motionWake && motionArmWom(WOM_MG);
  enterDeepSleep(g_set.motionWake ? 4 : 0);
}

// Woken by the timer: decide quickly whether to boot or go back to sleep.
static void quickWakeCheck() {
  if (!motionProbeOnly()) return;  // can't tell: do a full boot

  if (s_hotSleep) {                // cooling off: only wake once it's well below the limit
    float c;
    if (motionReadTemp(c) && c > g_set.hotOffC - 10) enterDeepSleep(300);
    s_hotSleep = false;
    return;
  }
  if (!s_haveRef) return;

  // Wake-on-motion: nothing moved since the last peek -> straight back to sleep (short peek).
  // Every 15th peek (~1 min) still takes a full look, in case the flag missed something slow.
  bool fired = false;
  if (s_womArmed) {
    fired = motionWomFired();
    bool buttons0 = boardBootPressed() || boardPwrPressed();
    if (!fired && !buttons0 && ++s_womQuiet < 15) { s_peeks++; enterDeepSleep(4); }
    s_womQuiet = 0;
    motionLowPower();               // readings again
    delay(120);                     // junk for ~100 ms after a mode switch
  }

  // 16 samples over ~0.6 s: mean shift = moved, spread = engine running
  float sum[3] = {0, 0, 0}, mags[16];
  int got = 0;
  for (int i = 0; i < 16; i++) {
    float x, y, z;
    if (motionReadAccel(x, y, z)) {
      sum[0] += x; sum[1] += y; sum[2] += z;
      mags[got++] = sqrtf(x * x + y * y + z * z);
    }
    delay(36);
  }
  if (!got) return;
  float mean = 0, var = 0;
  for (int i = 0; i < got; i++) mean += mags[i];
  mean /= got;
  for (int i = 0; i < got; i++) var += (mags[i] - mean) * (mags[i] - mean);
  float sd = sqrtf(var / got);
  float d = 0;
  for (int k = 0; k < 3; k++) d = fmaxf(d, fabsf(sum[k] / got - s_sleepRef[k]));
  s_peeks++; s_peekD = d; s_peekSd = sd; s_peekGot = got;
  for (int k = 0; k < 3; k++) s_peekMean[k] = sum[k] / got;

  bool vib = sd > g_set.engineVib * 0.7f;
  bool buttons = boardBootPressed() || boardPwrPressed();
  if (d < 0.035f && !vib && !buttons) {
    // it shook but is still where it was and no engine: a knock / door ding that's already over
    if (fired && g_set.guardOn) extrasGuardBump();
    peekSleep();
  }

  // Guard mode: it moved but no engine. Look again in a moment: if it has settled, the car was
  // only rocked (door ding, someone leaning on it): log a bump and go back to sleep. Still moving
  // or the engine started -> wake up properly.
  if (g_set.guardOn && !vib && !buttons) {
    delay(1200);
    float s2[3] = {0, 0, 0}, m2[16];
    int n2 = 0;
    for (int i = 0; i < 16; i++) {
      float x, y, z;
      if (motionReadAccel(x, y, z)) { s2[0] += x; s2[1] += y; s2[2] += z; m2[n2++] = sqrtf(x * x + y * y + z * z); }
      delay(36);
    }
    if (n2) {
      float mean2 = 0, var2 = 0, dd = 0;
      for (int i = 0; i < n2; i++) mean2 += m2[i];
      mean2 /= n2;
      for (int i = 0; i < n2; i++) var2 += (m2[i] - mean2) * (m2[i] - mean2);
      for (int k = 0; k < 3; k++) dd = fmaxf(dd, fabsf(s2[k] / n2 - sum[k] / got));
      bool settled = dd < 0.02f && sqrtf(var2 / n2) < g_set.engineVib * 0.7f;
      if (settled && !boardBootPressed() && !boardPwrPressed()) {
        for (int k = 0; k < 3; k++) s_sleepRef[k] = s2[k] / n2;   // its new resting position
        extrasGuardBump();
        peekSleep();
      }
    }
  }
  s_wokeByVib = vib;
}

static void showFor(uint32_t ms);

static void goToSleep() {
  if (g_set.summaryOn && tripShowSummary()) showFor(5000);
  if (extrasShowMilestone()) showFor(5000);
  brainEventPrio(Expr::Sleep, 4000, 99);
  showFor(1500);
  recorderSave();
  obdDisconnect();
  motionLowPower();
  delay(300);   // the QMI8658 outputs junk (+-4 g) for ~100 ms after a mode switch
  averageAccel(s_sleepRef, 10);
  s_haveRef = g_motion.ok;
  displayOff();
  powerLog("sleep");
  extrasOnSleep();
  s_womQuiet = 0;
  peekSleep();   // peek at the IMU every 4 s (wake-on-motion in between), or wait for a button/touch
}

static void hotSleep() {
  brainEventPrio(Expr::HotOff, 4000, 105);
  audioChirp(900, 400, 300);
  showFor(3000);
  recorderSave();
  motionLowPower();
  displayOff();
  s_hotSleep = true;
  s_womArmed = false;
  powerLog("hot");
  enterDeepSleep(300);  // recheck the temperature every 5 minutes
}

static void powerOff() {
  brainEventPrio(Expr::Sleep, 4000, 99);
  audioChirp(1200, 500, 250);
  showFor(1200);
  recorderSave();
  displayOff();
  powerLog("off");
  extrasOnSleep();
  boardPowerOff();   // on battery this cuts power right here
  delay(300);
  enterDeepSleep(0);  // on USB power: sleep until a button or touch
}

// --------------------------------------------------------------- frame ----
static bool s_night = false;
bool isNight() { return s_night; }   // voice.cpp: which brightness to change

static BrainCtx buildCtx() {
  BrainCtx c;
  if (musicHeard()) { c.playing = true; c.bpm = musicBpm(); }   // own mic (no radio on Otto)
  c.stoppedSecs = g_motion.stoppedSecs;
  c.hadDriven = g_motion.hadDriven;
  int h = localHour();
  c.night = h >= 0 && (h >= 19 || h < 7);
  s_night = c.night;
  c.batPct = powerPct();
  c.tempC = g_motion.tempC;
  c.calibProgress = g_motion.calibProgress;
  c.lon = g_motion.lon;
  c.rpm = obdRpm();
  c.coolantC = g_obd.coolAt ? g_obd.coolantC : -100;
  return c;
}

static void frame(uint32_t now, float dt, const BrainCtx& ctx) {
  float blink = 1;
  brainUpdate(now, ctx, s_target, blink);
  float k = 1 - expf(-dt * 10.0f);
  faceLerp(s_face, s_target, k);
  float extra = brainUserSpinStep(dt);
  s_crankAngle = fmodf(s_crankAngle + (s_face.spin + extra) * dt + 360.0f, 360.0f);

  bool hot = g_motion.tempC > g_set.overheatC;
  uint32_t interval = hot ? 100 : 33;  // 30 fps, 10 fps when hot
  if (now - s_lastFrame >= interval) {
    s_lastFrame = now;
    uint32_t t0 = micros();
    if (menuIsOpen()) menuRender(now);
    else faceRender(s_face, brainText(), s_crankAngle, blink, g_set.glow / 100.0f, now / 1000.0f);
    uint32_t t1 = micros();
    displayPush();
    extern uint32_t g_perfFrames, g_perfDrawUs, g_perfPushUs;
    g_perfFrames++; g_perfDrawUs += t1 - t0; g_perfPushUs += micros() - t1;
    if (g_bootLogN < BOOT_LOG && now < 8000)                 // `perf boot`: the first frames after power-on
      g_bootLog[g_bootLogN++] = {now, (uint16_t)((micros() - t0) / 100), (uint8_t)brainCurrent()};
  }
  int bri = ctx.night ? g_set.nightBri : g_set.dayBri;
  if (brainCurrent() == Expr::Sleep) bri = bri / 3;
  if (hot) bri = min(bri, 20);
  displayBacklight((uint8_t)(bri * 255 / 100));
  displayUpdateBacklight(dt);
}

static void showFor(uint32_t ms) {
  uint32_t start = millis(), last = start;
  BrainCtx ctx = buildCtx();
  while (millis() - start < ms) {
    uint32_t now = millis();
    frame(now, (now - last) / 1000.0f, ctx);
    last = now;
    delay(5);
  }
}

static void scheduleGreeting(uint32_t now, uint32_t delayMs, bool engineFirst) {
  if (engineFirst) brainEvent(Expr::EngineOn, 1600);
  s_greetAt = now + delayMs;
}

// ---------------------------------------------------------------- setup ---
void setup() {
  Serial.begin(115200);
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  boardEarlyInit();
  settingsLoad();
  if (cause == ESP_SLEEP_WAKEUP_TIMER) quickWakeCheck();
  boardReleasePinHolds();
  bool coldBoot = cause != ESP_SLEEP_WAKEUP_TIMER && cause != ESP_SLEEP_WAKEUP_EXT1;

  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  rtcInit();
  if (!shtc3Init()) Serial.println("SHTC3 (cabin temp) not found");
  if (!displayInit()) Serial.println("display init failed (SPD2010 / QSPI)");
  if (!touchInit()) Serial.println("touch not found");
  if (!motionInit()) Serial.println("IMU not found");
  powerInit();
  powerLog(coldBoot ? "boot" : "wake");
  if (!audioInit()) Serial.println("audio init failed");
  micInit();
  recorderInit();
  launchInit();
  extrasOnWake(coldBoot);
  voiceInit();
  brainInit();
  bleInit();
  obdInit();

  uint32_t now = millis();
  if (coldBoot) {
    brainEvent(Expr::Boot, 3200);            // outline 1.5 s, eyes at 1.9 s, pumping at 2.3 s
    audioChirp(400, 1200, 300);
    scheduleGreeting(now, 3300, false);
  } else if (s_wokeByVib) {
    scheduleGreeting(now, 1700, true);   // ENGINE ON, then the greeting
  } else {
    scheduleGreeting(now, 150, false);
  }
  s_wokeByVib = false;
  if (!coldBoot) {
    Serial.printf("sleep: woke by %s after %u motion checks\n",
                  cause == ESP_SLEEP_WAKEUP_TIMER ? "motion" : "button/touch", (unsigned)s_peeks);
    if (s_peeks)
      Serial.printf("sleep: last check %d samples, moved %.3f g, vib %.4f g (ref %.3f %.3f %.3f, now %.3f %.3f %.3f)\n",
                    s_peekGot, s_peekD, s_peekSd, s_sleepRef[0], s_sleepRef[1], s_sleepRef[2],
                    s_peekMean[0], s_peekMean[1], s_peekMean[2]);
  }
  s_peeks = 0; s_peekGot = -1;
  s_lastLoop = millis();
  Serial.println("OTTO " FW_VERSION " ready. Type 'help'.");
}

// ----------------------------------------------------------------- loop ---
static void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (s_serialLen) {
        s_serialLine[s_serialLen] = 0;
        handleCommand(s_serialLine, SRC_SERIAL);
        s_serialLen = 0;
      }
    } else if (s_serialLen < sizeof(s_serialLine) - 1) {
      s_serialLine[s_serialLen++] = c;
    }
  }
}

static bool idleish() {
  Expr e = brainCurrent();
  return e == Expr::Idle || e == Expr::Bored || e == Expr::MusicBob;
}

uint32_t g_perfLoops = 0, g_perfLoopMaxUs = 0;   // `perf`
uint32_t g_perfFrames = 0, g_perfDrawUs = 0, g_perfPushUs = 0;
BootFrame g_bootLog[BOOT_LOG];
int g_bootLogN = 0;

void loop() {
  static uint32_t s_loopT = 0;
  uint32_t lt = micros();
  if (s_loopT) { uint32_t d = lt - s_loopT; if (d > g_perfLoopMaxUs) g_perfLoopMaxUs = d; }
  s_loopT = lt;
  g_perfLoops++;
  uint32_t now = millis();
  float dt = (now - s_lastLoop) / 1000.0f;
  if (dt > 0.1f) dt = 0.1f;
  s_lastLoop = now;

  readSerial();
  bleUpdate(now);
  inputUpdate(now);
  touchUpdate(now);
  menuUpdate(now);
  motionUpdate(now);
  powerUpdate(now);
  launchUpdate(now);
  tripUpdate(now, dt);
  otaUpdate(now);
  obdUpdate(now);

  // greeting (on boot / wake / engine start)
  if (s_greetAt && (int32_t)(now - s_greetAt) >= 0) {
    s_greetAt = 0;
    s_lastGreet = now;
    if (!s_bootDone) s_bootDone = now;
    if (!g_set.calibrated && g_motion.ok) motionStartCalibration();  // first mount
    else if (!extrasGreet()) greetNow();   // 7+ days parked: "Lange nicht gesehen!" instead
  }

  // engine start / stop while awake
  if (g_motion.engineStarted) {
    g_motion.engineStarted = false;
    if (s_bootDone && now - s_lastGreet > 600000 && !menuIsOpen()) scheduleGreeting(now, 1700, true);
    else if (s_bootDone && !s_greetAt && now - s_lastGreet > 30000) brainEvent(Expr::EngineOn, 1500);
  }
  if (g_motion.engineStopped) {
    g_motion.engineStopped = false;
    if (g_set.summaryOn) tripShowSummary();
  }

  // battery level once per wake, after the greeting (ChargeSoon instead when it's getting low)
  if (s_batPending && s_bootDone && now - s_bootDone > 2800 && !s_greetAt && idleish()) {
    s_batPending = false;
    int p = powerPct();
    if (!powerCharging() && p <= 25 && p >= 15) { s_chargeSoonShown = true; brainEvent(Expr::ChargeSoon, 3000); audioChirp(700, 500, 150); }
    else brainEvent(Expr::BatteryLevel, 1800);
  }

  // everyday extras: guard / cabin cards after the battery level, milestones, break time, show mode
  extrasUpdate(now, s_bootDone && now - s_bootDone > 4000 && !s_greetAt && !s_batPending && idleish());

  // power + temperature events
  bool chg = powerCharging();
  if (chg && !s_wasCharging) brainEvent(Expr::Charging, 3000);
  s_wasCharging = chg;
  int pct = powerPct();
  if (!chg && pct <= 25 && pct >= 15 && !s_chargeSoonShown && !s_batPending) {   // crossed 25 % while awake
    s_chargeSoonShown = true;
    brainEvent(Expr::ChargeSoon, 3000);
    audioChirp(700, 500, 150);
  }
  if (chg || pct > 30) s_chargeSoonShown = false;
  bool full = powerFull();
  if (full && !s_wasFull) { brainEvent(Expr::Charged, 4000); audioChirp(900, 1800, 200); }
  s_wasFull = full;
  if (!chg && pct < 15 && now - s_lastLowBatt > 90000) { s_lastLowBatt = now; brainEvent(Expr::LowBattery, 4000); }
  if (!chg && powerVolts() < 3.35f && powerVolts() > 2.5f) s_wantOff = true;  // protect the cell
  if (g_motion.tempC > g_set.overheatC && now - s_lastHot > 30000) { s_lastHot = now; brainEvent(Expr::Overheat, 4000); }
  if (g_motion.tempC > g_set.hotOffC && !otaActive()) {
    if (!s_hotSince) s_hotSince = now;
    if (now - s_hotSince > 10000) hotSleep();
  } else s_hotSince = 0;

  // "Sehr gut": Otto approves of 10 minutes of smooth driving (no hard braking, launches, bumps)
  static float s_smoothSecs = 0;
  {
    Expr cur = brainCurrent();
    bool harsh = cur == Expr::Braking || cur == Expr::Accel || cur == Expr::Bump || cur == Expr::Dizzy ||
                 cur == Expr::V8 || cur == Expr::Rev || launchRunning();
    if (harsh || !g_motion.ok) s_smoothSecs = 0;
    else if (g_motion.stoppedSecs < 0.5f && g_motion.hadDriven) s_smoothSecs += dt;
    if (s_smoothSecs > 600 && idleish() && !menuIsOpen()) {
      s_smoothSecs = 0;
      brainEvent(Expr::SehrGut, 3500);
      audioPlayClip("sehrgut");
    }
  }

  shtc3Update(now);
  static uint32_t s_lastBatLog = 0;
  if (now - s_lastBatLog >= 300000) { s_lastBatLog = now; if (now > 60000) powerLog("awake"); }
  voiceUpdate(now);
  musicUpdate(now);
  BrainCtx ctx = buildCtx();
  frame(now, dt, ctx);

  if (now - s_lastTelem >= 250) {
    s_lastTelem = now;
    char b[240];
    buildTelemetry(b, sizeof(b));
    bleNotifyTelemetry(b);
  }

  // sleep when parked with nothing connected
  bool busy = menuIsOpen() || otaActive() || touchActive() || launchRunning() || extrasShowOn();
  if (g_motion.ok && g_motion.stillSecs > g_set.sleepMin * 60 && !bleAnyConnected() && !busy) s_wantSleep = true;

  if (s_wantOff && !otaActive()) powerOff();
  if (s_wantSleep && !otaActive()) goToSleep();
  s_wantSleep = false;
  delay(1);
}
