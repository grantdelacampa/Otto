#include "commands.h"
#include <Wire.h>
#include <ctype.h>
#include "config.h"
#include "ble.h"
#include <NimBLEDevice.h>
#include "brain.h"
#include "board.h"
#include "motion.h"
#include "power.h"
#include "audio.h"
#include "settings.h"
#include "recorder.h"
#include "greetings.h"
#include "launch.h"
#include "trip.h"
#include "touch.h"
#include "vibcap.h"
#include "mic.h"
#include "voice.h"
#include "extras.h"
#include "holidays.h"
#include "music.h"
#include "obd.h"
#include "wifitune.h"
#include "pwrtest.h"
#include "drivelog.h"

SpeedInput g_speed;
void requestSleep();  // main.cpp

void wifiTuneReply(const char* msg);   // wifitune.cpp

static void reply(uint16_t src, const char* msg) {
  if (src == SRC_HTTP) { wifiTuneReply(msg); return; }
  if (src == SRC_SERIAL) Serial.println(msg);
  else bleReply(src, msg);
}

static const char* arg(const char* line, const char* cmd) {
  size_t n = strlen(cmd);
  if (strncasecmp(line, cmd, n)) return nullptr;
  if (line[n] == 0) return line + n;
  if (line[n] != ' ') return nullptr;
  return line + n + 1;
}

// face tester: sample data for the card screens
static bool cardDemo(Expr e, uint32_t ms) {
  switch (e) {
    case Expr::LaunchRun:    brainSetAnswer("0-60", "42", "MPH - 2.9 S"); brainSetRing(0.7f); break;
    case Expr::LaunchResult: brainSetAnswer("0-60", "5.8 S", "NEW BEST!"); break;
    case Expr::Summary:      brainSetAnswer("DRIVE - 24 MIN", "92", "12.4 MI - 0.6G TURN"); break;
    case Expr::Updating:     brainSetRing(0.64f); break;
    case Expr::MenuOpening:  brainSetRing(0.6f); break;
    case Expr::LaunchReady:  brainSetCaption("0-60 ARMED"); break;
    case Expr::CheckEngine:  brainSetAnswer("CHECK ENGINE", "P0171", "CODE 1 OF 2 - LAMP ON"); break;
    case Expr::WifiTune:     brainSetAnswer("WI-FI OTTO-430C", "otto1a2b", "THEN OPEN 192.168.4.1"); break;
    // faces whose words come from the code that triggers them: without these `expr` showed
    // whatever text the last card left behind ("LET'S GO" on the break card, bench Nov 15 2026)
    case Expr::LaunchTree:   brainSetCaption("TREE .500"); break;
    case Expr::LaunchGo:     brainSetGreeting("Los geht's!", "LET'S GO", Eye::Chevron); break;
    case Expr::LaunchRed:    brainSetCaption("RED LIGHT!"); break;
    case Expr::LaunchRecord: brainSetGreeting("Neuer Rekord!", "NEW RECORD", Eye::Happy); brainSetCaption("0-60 5.8 S"); break;
    case Expr::Guard:        brainSetAnswer("SOMEONE BUMPED ME", "3:42 PM", "2 BUMPS"); break;
    case Expr::MissedYou:    brainSetGreeting("Lange nicht gesehen!", "LONG TIME NO SEE", Eye::Happy); brainSetCaption("8 DAYS"); break;
    case Expr::Milestone:    brainSetAnswer("WE MADE IT", "100", "DRIVES TOGETHER"); break;
    case Expr::BreakTime:    brainSetGreeting("Pause?", "TIME FOR A BREAK?", Eye::Content); brainSetCaption("2 H 10 MIN"); break;
    case Expr::HotCabin:     brainSetGreeting("Heiß!", "54 C INSIDE", Eye::Half); brainSetCaption("AIR IT OUT"); break;
    case Expr::ColdCabin:    brainSetGreeting("Kalt!", "2 C INSIDE", Eye::Dot); brainSetCaption("BRRR!"); break;
    case Expr::ShowCard:     brainSetAnswer(g_set.showL1, g_set.showL2, "TAP ME!"); break;
    default: return false;
  }
  brainEventPrio(e, ms, 120);
  return true;
}

size_t buildTelemetry(char* b, size_t n) {
  Peaks p = recorderPeaks();
  return snprintf(b, n,
                  "{\"bat\":%d,\"mv\":%d,\"chg\":%d,\"tc\":%.1f,\"lon\":%.2f,\"lat\":%.2f,\"vert\":%.2f,"
                  "\"yaw\":%.0f,\"ex\":\"%s\",\"cal\":%d,\"pk\":[%.2f,%.2f,%.2f,%.0f,%.2f],"
                  "\"vib\":%.3f,\"eng\":%d,\"best\":%.1f,\"obd\":%d,\"rpm\":%.0f,\"cool\":%.0f}",
                  powerPct(), (int)(powerVolts() * 1000), powerCharging(), g_motion.tempC, g_motion.lon,
                  g_motion.lat, g_motion.vert, g_motion.yaw, exprName(brainCurrent()),
                  g_set.calibrated, p.brake, p.accel, p.lat, p.yaw, p.bump,
                  g_motion.vib, g_motion.engineOn, launchBest(),
                  g_obd.ecu ? 2 : g_obd.adapter ? 1 : 0, obdRpm(), g_obd.coolAt ? g_obd.coolantC : -100.0f);
}

void handleCommand(const char* raw, uint16_t src) {
  char line[240];
  strncpy(line, raw, sizeof(line) - 1);
  line[sizeof(line) - 1] = 0;
  // trim
  size_t L = strlen(line);
  while (L && (line[L - 1] == '\n' || line[L - 1] == '\r' || line[L - 1] == ' ')) line[--L] = 0;
  if (!L) return;
  const char* a;

  if (!strncasecmp(line, "obd<", 4)) {          // bench: a reply from tools/fake_obd.py
    obdSimRx(line[4] == ' ' ? line + 5 : line + 4);
  } else if ((a = arg(line, "obd"))) {
    char b[240];
    obdCommand(a, b, sizeof(b));
    reply(src, b);
  } else if ((a = arg(line, "hello"))) {
    reply(src, src == SRC_SERIAL ? "hi from OTTO " FW_VERSION : "hello " FW_VERSION);
  } else if ((a = arg(line, "voice"))) {
    Expr e;
    if (exprFromName(a, e)) brainVoice(e);
  } else if ((a = arg(line, "expr"))) {
    char name[24] = "";
    int ms = 3000;
    sscanf(a, "%23s %d", name, &ms);
    Expr e;
    if (exprFromName(name, e) && cardDemo(e, ms)) { reply(src, "ok"); }
    else if (exprFromName(name, e)) { brainEventPrio(e, ms, 95); reply(src, "ok"); }
    else reply(src, "unknown expression");
  } else if ((a = arg(line, "speed"))) {           // external speed in mph (nothing sends it yet; tools/fake_gps.py on the bench)
    if (!*a) {                                     // bare `speed`: what did we last get?
      char b[64];
      snprintf(b, sizeof(b), "speed %.1f mph, %ld ms ago", g_speed.mph,
               g_speed.at ? (long)(millis() - g_speed.at) : -1L);
      reply(src, b);
    } else {
      g_speed.mph = fmaxf(0.0f, (float)atof(a));
      g_speed.at = millis();
    }
  } else if ((a = arg(line, "launch"))) {
    if (!strcasecmp(a, "arm")) { launchArm(); reply(src, "launch armed"); }
    else if (!strcasecmp(a, "resetbest")) { launchResetBest(); reply(src, "best cleared"); }
  } else if (arg(line, "summary")) {
    if (!tripShowSummary()) reply(src, "no drive yet (needs 3+ min)");
  } else if ((a = arg(line, "wifi"))) {          // Wi-Fi tuning mode: wifi [on|off]
    if (!strcasecmp(a, "on")) wifiTuneStart();
    else if (!strcasecmp(a, "off")) wifiTuneStop();
    char b[160];
    wifiTuneInfo(b, sizeof(b));
    reply(src, b);
  } else if (arg(line, "sleeplog")) {            // debug: the last motion checks while asleep
    extern void sleepLogPrint(void (*)(const char*));
    static uint16_t s_src; s_src = src;
    sleepLogPrint([](const char* m) { reply(s_src, m); });
  } else if (arg(line, "awake")) {               // debug: what keeps Otto from sleeping
    extern float g_dbgMagVar, g_dbgGyro;
    extern bool menuIsOpen(); extern bool otaActive();
    char b[220];
    snprintf(b, sizeof(b), "awake: still %.0f of %d s (accel var %.6f < 0.000150, gyro %.2f < 1.50 dps), ble %d, menu %d, ota %d, touch %d, launch %d, show %d, wifi %d, imu %d",
             g_motion.stillSecs, g_set.sleepMin * 60, g_dbgMagVar, g_dbgGyro, bleAnyConnected(), menuIsOpen(), otaActive(),
             touchActive(), launchRunning(), extrasShowOn(), wifiTuneActive(), g_motion.ok);
    reply(src, b);
  } else if (arg(line, "mem")) {                 // debug: free memory (internal RAM matters for Wi-Fi + voice)
    char b[160];
    snprintf(b, sizeof(b), "mem: internal %u free (largest block %u, lowest ever %u), PSRAM %u free",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    reply(src, b);
  } else if (arg(line, "reboot")) {              // restart the board (bench)
    reply(src, "rebooting");
    delay(100);
    ESP.restart();
  } else if (arg(line, "perf boot")) {           // the first frames after power-on: time, gap, draw+push, face
    char b[64];
    for (int i = 0; i < g_bootLogN; i++) {
      snprintf(b, sizeof(b), "%5u ms  +%4u  %5.1f ms  %s", (unsigned)g_bootLog[i].t,
               i ? (unsigned)(g_bootLog[i].t - g_bootLog[i - 1].t) : 0, g_bootLog[i].ms10 / 10.0f, exprName((Expr)g_bootLog[i].expr));
      reply(src, b);
    }
  } else if (arg(line, "perf")) {                // debug: loop rate + touch read timing since the last `perf`
    extern uint32_t g_perfLoops, g_perfLoopMaxUs, g_tpReads, g_tpReadUs, g_tpReadMaxUs, g_tpGapMaxMs;
    extern uint32_t g_perfFrames, g_perfDrawUs, g_perfPushUs;
    static uint32_t s_since = 0;
    uint32_t now = millis(), span = s_since ? now - s_since : now;
    char b[200];
    snprintf(b, sizeof(b), "perf: %.1f s, loop %.0f/s (longest %.1f ms), touch reads %u (avg %.2f ms, longest %.2f ms), longest gap while touched %u ms",
             span / 1000.0f, g_perfLoops * 1000.0f / (span ? span : 1), g_perfLoopMaxUs / 1000.0f, (unsigned)g_tpReads,
             g_tpReads ? g_tpReadUs / 1000.0f / g_tpReads : 0.0f, g_tpReadMaxUs / 1000.0f, (unsigned)g_tpGapMaxMs);
    reply(src, b);
    snprintf(b, sizeof(b), "perf: %u frames, draw %.1f ms, push %.1f ms per frame", (unsigned)g_perfFrames,
             g_perfFrames ? g_perfDrawUs / 1000.0f / g_perfFrames : 0.0f, g_perfFrames ? g_perfPushUs / 1000.0f / g_perfFrames : 0.0f);
    reply(src, b);
    extern uint32_t g_perfPart[5];
    if (g_perfFrames) {
      float k = 1.0f / 1000.0f / g_perfFrames;
      snprintf(b, sizeof(b), "perf: draw parts - clear %.1f, cylinder %.1f, piston+mouth %.1f, brows+eyes %.1f, overlays %.1f ms",
               g_perfPart[0] * k, g_perfPart[1] * k, g_perfPart[2] * k, g_perfPart[3] * k, g_perfPart[4] * k);
      reply(src, b);
    }
    extern uint32_t g_pcRedraws;
    extern uint32_t g_pcWhy[6]; snprintf(b, sizeof(b), "perf: piston cache redrawn %u times (lean %u, size %u, scale %u, hue %u, glow strength %u, glow %u)", (unsigned)g_pcRedraws, (unsigned)g_pcWhy[0], (unsigned)g_pcWhy[1], (unsigned)g_pcWhy[2], (unsigned)g_pcWhy[3], (unsigned)g_pcWhy[4], (unsigned)g_pcWhy[5]); memset(g_pcWhy, 0, sizeof(g_pcWhy));
    reply(src, b);
    g_pcRedraws = 0;
    memset(g_perfPart, 0, sizeof(g_perfPart));
    g_perfFrames = g_perfDrawUs = g_perfPushUs = 0;
    g_perfLoops = g_perfLoopMaxUs = g_tpReads = g_tpReadUs = g_tpReadMaxUs = g_tpGapMaxMs = 0;
    s_since = now;
  } else if (!strncasecmp(line, "touch int", 9)) {   // debug: INT line activity like during sleep: touch int [secs]
    float secs = line[9] ? atof(line + 9) : 10;
    touchPrepareSleep();                         // pauses the reader + clears INT, as before deep sleep
    pinMode(PIN_TP_INT, INPUT_PULLUP);
    delay(30);
    uint32_t t0 = millis(), until = t0 + (uint32_t)(constrain(secs, 1.0f, 60.0f) * 1000);
    int last = digitalRead(PIN_TP_INT), lows = 0;
    Serial.printf("touch int: start level %d\n", last);
    uint32_t lowAt = 0;
    while (millis() < until) {
      int v = digitalRead(PIN_TP_INT);
      if (v != last) {
        if (v == LOW) {
          lows++; lowAt = micros();
          if (lows <= 20) Serial.printf("  low at %lu ms\n", millis() - t0);
          if (strstr(line, "read") && lows <= 6) { TouchPoint p[1]; touchRawRead(p); }   // service it: status printed
        }
        else if (lows <= 20) Serial.printf("  high again after %lu us\n", micros() - lowAt);
        last = v;
      }
    }
    char b[80]; snprintf(b, sizeof(b), "touch int: %d low pulses in %.0f s", lows, secs); reply(src, b);
    touchResume();
  } else if (!strncasecmp(line, "touch raw", 9)) {   // debug: every SPD2010 status read: touch raw [secs]
    float secs = line[9] ? atof(line + 9) : 10;
    uint32_t until = millis() + (uint32_t)(constrain(secs, 1.0f, 60.0f) * 1000);
    while (millis() < until) {
      TouchPoint p[1];
      int n = touchRawRead(p);
      if (n > 0) Serial.printf("   -> (%d,%d)\n", p[0].x, p[0].y);
      else Serial.printf("   -> n %d\n", n);
      delay(15);
    }
  } else if ((a = arg(line, "touch"))) {         // debug: print raw touch points: touch [secs] (default 5)
    float secs = *a ? atof(a) : 5;
    uint32_t until = millis() + (uint32_t)(constrain(secs, 1.0f, 60.0f) * 1000);
    int lastN = 0;
    while (millis() < until) {
      TouchPoint p[5];
      int n = touchDebugRead(p, 5);
      if (n == 0 && lastN != 0) reply(src, "0: lifted");
      if (n >= 0) lastN = n;
      if (n > 0) {
        char b[96];
        int k = snprintf(b, sizeof(b), "%d:", n);
        for (int i = 0; i < n && k < 80; i++) k += snprintf(b + k, sizeof(b) - k, " (%d,%d)", p[i].x, p[i].y);
        reply(src, b);
      }
      delay(30);
    }
  } else if ((a = arg(line, "volume"))) {
    brainSetVolume(atoi(a));
    brainEvent(Expr::Volume, 2000);
  } else if ((a = arg(line, "answer"))) {
    char b[120];
    strncpy(b, a, sizeof(b) - 1); b[sizeof(b) - 1] = 0;
    char* p1 = strchr(b, '|');
    char* p2 = p1 ? strchr(p1 + 1, '|') : nullptr;
    if (p1) *p1++ = 0;
    if (p2) *p2++ = 0;
    brainSetAnswer(b, p1 ? p1 : "", p2 ? p2 : "");
    brainEvent(Expr::Answer, 5000);
  } else if (arg(line, "confused")) {
    brainEvent(Expr::Confused, 2000);
  } else if ((a = arg(line, "time"))) {
    long long epoch = 0; int tz = tzOffsetMin();
    sscanf(a, "%lld %d", &epoch, &tz);
    if (epoch > 1700000000LL) { rtcSetUtc((time_t)epoch); setTzOffsetMin(tz); reply(src, "time set"); }
  } else if (arg(line, "calibrate")) {
    motionStartCalibration();
  } else if (arg(line, "flipfwd")) {
    motionFlipForward();
    reply(src, g_set.fwdSign > 0 ? "forward: normal" : "forward: flipped");
  } else if (arg(line, "greet")) {
    greetNow();
  } else if (arg(line, "log")) {
    if (src == SRC_SERIAL) {
      Serial.println("sec,brake,accel,lat,yaw,bump");
      for (size_t i = 0; i < recorderCount(); i++) {
        LogRec r = recorderGet(i);
        Serial.printf("%u,%.3f,%.3f,%.3f,%.1f,%.3f\n", (unsigned)i, r.brake / 1000.0f, r.accel / 1000.0f,
                      r.lat / 1000.0f, r.yaw / 10.0f, r.bump / 1000.0f);
      }
    } else bleStartLogStream();
  } else if (arg(line, "resetpeaks")) {
    recorderResetPeaks();
    reply(src, "peaks cleared");
  } else if (arg(line, "get")) {
    reply(src, settingsToJson().c_str());
  } else if ((a = arg(line, "cfg"))) {
    reply(src, settingsApplyJson(a) ? "saved" : "bad json");
  } else if (arg(line, "reset")) {
    settingsReset();
    reply(src, "settings reset");
  } else if ((a = arg(line, "womcheck"))) {      // debug: womcheck [reset] - the sleep motion check on the bench
    motionWomCheck(strstr(a, "reset") != nullptr, 6);
  } else if ((a = arg(line, "wom"))) {           // debug: wom [mg] [CTRL2 hex] [secs]
    unsigned mg = 60, c2 = 0x1D, secs = 20;
    sscanf(a, "%u %x %u", &mg, &c2, &secs);
    motionWomTest(mg, c2, secs);
    reply(src, "wom done");
  } else if ((a = arg(line, "imulp"))) {         // debug: IMU in pre-sleep low-power mode
    unsigned c2 = 0x18, c5 = 0xFF;                // optional: imulp <CTRL2 hex> <CTRL5 hex>
    sscanf(a, "%x %x", &c2, &c5);
    motionLowPowerTest((uint8_t)c2, (uint8_t)c5);
    uint32_t t0 = millis();
    for (int i = 0; i < 24; i++) {
      float x = 0, y = 0, z = 0;
      bool ok = motionReadAccel(x, y, z);
      char b[72];
      snprintf(b, sizeof(b), "lp %4lu ms: %s %.3f %.3f %.3f |%.3f|", (unsigned long)(millis() - t0),
               ok ? "ok" : "FAIL", x, y, z, sqrtf(x * x + y * y + z * z));
      reply(src, b);
      delay(50);
    }
    motionInit();                                // back to normal mode
  } else if ((a = arg(line, "listen"))) {        // voice commands: listen [on|off|now|nap|thresh 0.x]  (bare = status)
    char b[160];
    if (!strcasecmp(a, "on") || !strcasecmp(a, "off")) {
      g_set.voiceOn = !strcasecmp(a, "on"); settingsSave(); voiceSetEnabled(g_set.voiceOn);
    } else if (!strcasecmp(a, "now")) voiceListenNow();
    else if (!strcasecmp(a, "nap")) voiceNapNow();
    else if (!strncasecmp(a, "thresh", 6)) voiceSetThreshold(atof(a + 6));
    voiceStatus(b, sizeof(b));
    reply(src, b);
  } else if ((a = arg(line, "show"))) {          // car show mode: show on|off
    extrasShow(!strcasecmp(a, "on"));
    reply(src, extrasShowOn() ? "show mode on" : "show mode off");
  } else if ((a = arg(line, "extras"))) {        // everyday extras status / bench demos
    char b[160];
    extrasDebug(a, b, sizeof(b));
    reply(src, b);
  } else if ((a = arg(line, "batfake"))) {       // bench: batfake 3.75 (volts) / batfake off
    float v = atof(a);
    powerFake(v >= 2.5f && v <= 4.5f ? v : 0);
    reply(src, v >= 2.5f && v <= 4.5f ? "battery reading faked" : "battery reading real");
  } else if ((a = arg(line, "pwrtest"))) {       // bench power test on battery: pwrtest start [secs] | stop
    if (!strncasecmp(a, "start", 5)) { int d = atoi(a + 5); pwrtestStart((d > 0 ? d : 30) * 1000UL); reply(src, "pwrtest: unplug USB; results after the restart (pwrtest)"); }
    else if (!strcasecmp(a, "stop")) pwrtestStop();
    else pwrtestReport(src, reply);
  } else if ((a = arg(line, "phone"))) {         // phone clock: last result; "phone forget" = drop all pairings
    if (!strcasecmp(a, "forget") && bleReady()) { NimBLEDevice::deleteAllBonds(); reply(src, "phone: pairings forgotten (also Forget OTTO on the phone)"); }
    else reply(src, blePhoneStatus());
  } else if ((a = arg(line, "autooff"))) {        // bench: autooff <secs> = power off after this long asleep (once)
    extern void setAutoOffTest(uint32_t);
    int secs = atoi(a);
    setAutoOffTest(secs > 0 ? secs : 0);
    char b[80];
    snprintf(b, sizeof(b), "autooff: %d s asleep (normal: 3600); unplug USB, it can't power off on USB", secs > 0 ? secs : 3600);
    reply(src, b);
  } else if ((a = arg(line, "womint"))) {        // bench: does the IMU knock alarm reach GPIO0 (wake pin)?
    static uint16_t s_src; s_src = src;
    int secs = atoi(a);
    motionWomIntTest(secs > 0 ? secs : 15, [](const char* m) { reply(s_src, m); });
  } else if ((a = arg(line, "enginelearn"))) {   // = menu ENGINE LEARN; "road" = voice "learn the road"; "tap" = a tap (stop / skip)
    if (!strcasecmp(a, "tap")) motionEngineLearnTap();
    else if (!strcasecmp(a, "road")) motionStartRoadLearn();       // = voice "learn the road"
    else motionStartEngineLearn();
    reply(src, motionEngineLearning() ? "engine learn: running (serial shows each step)" : "engine learn: not running");
  } else if ((a = arg(line, "drivelog"))) {      // drive diagnostics: drivelog | drivelog clear
    if (!strcasecmp(a, "clear")) { drivelogClear(); reply(src, "drivelog cleared"); }
    else drivelogDump(src, reply);
  } else if ((a = arg(line, "batlog"))) {        // battery log: batlog [mAh] | batlog clear
    if (!strcasecmp(a, "clear")) { powerLogClear(); reply(src, "batlog cleared"); }
    else powerLogDump(src, reply, atof(a));
  } else if ((a = arg(line, "music"))) {         // own-mic music detection: music [watch secs | loud|beat|steady v]
    char w[16] = ""; float v = 0;
    int k = sscanf(a, "%15s %f", w, &v);
    if (k >= 1 && !strcasecmp(w, "watch")) { musicWatch(k == 2 ? v : 30); reply(src, "music: watching (serial)"); }
    else if (k == 2) musicSetThresholds(w, v);
    char b[200];
    musicStatus(b, sizeof(b));
    reply(src, b);
  } else if ((a = arg(line, "mic"))) {           // mic level test: mic [secs]
    micLevelTest(atof(a), src, reply);
  } else if ((a = arg(line, "vibcap"))) {        // idle vibration test: vibcap [csv|raw]
    vibcapRun(a, src, src == SRC_SERIAL, reply);
  } else if (arg(line, "i2cscan")) {             // debug: list devices on the I2C bus (touch shares it)
    char b[160];
    int k = snprintf(b, sizeof(b), "Wire (main):");
    for (uint8_t ad = 1; ad < 127 && k < 150; ad++) {
      Wire.beginTransmission(ad);
      if (Wire.endTransmission() == 0) k += snprintf(b + k, sizeof(b) - k, " 0x%02X", ad);
    }
    reply(src, b);
  } else if ((a = arg(line, "play")) && *a) {   // play a voice clip by name, e.g. "play okaeri"
    audioPlayClip(a);
    reply(src, "ok");
  } else if (arg(line, "clock")) {               // show Otto's local date/time (from the RTC)
    time_t utc = rtcNowUtc();
    if (!rtcValid() || !utc) { reply(src, "clock not set"); return; }
    time_t loc = utc + tzOffsetMin() * 60;
    struct tm t;
    gmtime_r(&loc, &t);
    char b[48];
    snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d (tz %+d min)", t.tm_year + 1900, t.tm_mon + 1,
             t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, tzOffsetMin());
    reply(src, b);
  } else if (arg(line, "stat")) {
    char b[240];
    buildTelemetry(b, sizeof(b));
    reply(src, b);
  } else if ((a = arg(line, "birthday"))) {
    // birthday M D   — set birthday month and day
    // birthday clear — clear birthday
    if (!strcasecmp(a, "clear")) {
      g_set.birthdayMonth = 0; g_set.birthdayDay = 0;
      settingsSave(); reply(src, "birthday cleared");
    } else {
      int m = 0, d = 0;
      sscanf(a, "%d %d", &m, &d);
      if (m >= 1 && m <= 12 && d >= 1 && d <= 31) {
        g_set.birthdayMonth = m; g_set.birthdayDay = d;
        settingsSave();
        char b[32]; snprintf(b, sizeof(b), "birthday set: %d/%d", m, d);
        reply(src, b);
      } else reply(src, "usage: birthday M D  (e.g. birthday 7 14)");
    }
  } else if ((a = arg(line, "holidays"))) {
    if (!strcasecmp(a, "on"))  { g_set.holidaysOn = true;  settingsSave(); reply(src, "holidays on"); }
    else if (!strcasecmp(a, "off")) { g_set.holidaysOn = false; settingsSave(); reply(src, "holidays off"); }
    else if (!*a) {                                // bare "holidays": today's status
      static const char* kHol[] = {"none", "newyear", "nye", "valentines", "july4", "halloween",
                                   "thanksgiving", "christmaseve", "christmas", "birthday", "carbirthday"};
      int m = 0, d = 0;
      Holiday h = holidayToday(&m, &d), c = holidayCostumeType();
      char b[96];
      snprintf(b, sizeof(b), "holidays %s - today %d/%d: %s - costume: %s", g_set.holidaysOn ? "on" : "off",
               m, d, kHol[(int)h], kHol[(int)c]);
      reply(src, b);
    }
    else reply(src, "usage: holidays [on|off]");
  } else if (arg(line, "sleep")) {
    requestSleep();
  } else if (arg(line, "boop")) {
    brainEvent(Expr::Boop, 1200);
    audioChirp(900, 1500, 120);
  } else if (arg(line, "help")) {
    reply(src, "hello voice expr obd wifi mem perf reboot speed launch summary volume answer "
               "confused time clock imulp wom i2cscan vibcap mic music listen batlog batfake pwrtest drivelog enginelearn autooff show extras play calibrate flipfwd greet log resetpeaks get cfg reset stat touch "
               "sleep boop birthday holidays");
  } else {
    reply(src, "unknown command");
  }
}
