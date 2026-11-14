#include "commands.h"
#include <Wire.h>
#include <ctype.h>
#include "config.h"
#include "ble.h"
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

SpeedInput g_speed;
void requestSleep();  // main.cpp

static void reply(uint16_t src, const char* msg) {
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
                  "\"vib\":%.3f,\"eng\":%d,\"best\":%.1f,\"cab\":%.1f,\"rh\":%.0f}",
                  powerPct(), (int)(powerVolts() * 1000), powerCharging(), g_motion.tempC, g_motion.lon,
                  g_motion.lat, g_motion.vert, g_motion.yaw, exprName(brainCurrent()),
                  g_set.calibrated, p.brake, p.accel, p.lat, p.yaw, p.bump,
                  g_motion.vib, g_motion.engineOn, launchBest(),
                  shtc3Ok() ? shtc3TempC() : -99.0f, shtc3Ok() ? shtc3Humidity() : -1.0f);
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

  if ((a = arg(line, "hello"))) {
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
  } else if (arg(line, "touch")) {               // debug: print raw touch points for 5 s
    uint32_t until = millis() + 5000;
    while (millis() < until) {
      TouchPoint p[5];
      int n = touchDebugRead(p, 5);
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
  } else if ((a = arg(line, "listen"))) {        // voice commands: listen [on|off|now|thresh 0.x]  (bare = status)
    char b[96];
    if (!strcasecmp(a, "on") || !strcasecmp(a, "off")) {
      g_set.voiceOn = !strcasecmp(a, "on"); settingsSave(); voiceSetEnabled(g_set.voiceOn);
    } else if (!strcasecmp(a, "now")) voiceListenNow();
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
    reply(src, "hello voice expr speed launch summary volume answer "
               "confused time clock imulp wom i2cscan vibcap mic music listen batlog batfake show extras play calibrate flipfwd greet log resetpeaks get cfg reset stat touch "
               "sleep boop birthday holidays");
  } else {
    reply(src, "unknown command");
  }
}
