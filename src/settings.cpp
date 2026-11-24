#include "settings.h"
#include <stddef.h>
#include <Preferences.h>
#include <ArduinoJson.h>

Settings g_set;
static const uint32_t SETTINGS_VERSION = 8;   // Otto starts its own numbering (own NVS namespace)
// Versions 3 and 4 only appended fields, so an older blob is a prefix of today's: load it over the
// defaults and the owner keeps everything (mount calibration included).
// v2 ended on an int (no tail padding); v3 ended on a bool, so its blob carries padding up to the
// struct's alignment: copy only the bytes v3 really had.
static const size_t V2_SIZE = offsetof(Settings, wirelessOn);
static const size_t V3_USED = offsetof(Settings, wirelessOn) + sizeof(bool);
static const size_t V3_SIZE = (V3_USED + alignof(Settings) - 1) / alignof(Settings) * alignof(Settings);
static const size_t V4_SIZE = offsetof(Settings, idleGyro);   // v4 ended on a float: no padding
static const size_t V5_SIZE = offsetof(Settings, idleNoiseDb);   // v5 ended on a float too
static const size_t V6_SIZE = offsetof(Settings, screenFlip);    // v6 ended on a float too
static const size_t V7_SIZE = offsetof(Settings, engineFelt);    // v7 ended on an int

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int   clampi(int v, int lo, int hi)       { return v < lo ? lo : (v > hi ? hi : v); }

static void sanitize(Settings& s) {
  s.brake = clampf(s.brake, 0.05f, 1.5f);
  s.accel = clampf(s.accel, 0.05f, 1.5f);
  s.turn  = clampf(s.turn, 5, 120);
  s.bump  = clampf(s.bump, 0.2f, 3.0f);
  s.v8    = clampf(s.v8, 0.1f, 2.0f);
  s.activity = clampi(s.activity, 1, 10);
  s.sleepMin = clampi(s.sleepMin, 1, 30);
  s.dayBri   = clampi(s.dayBri, 5, 100);
  s.nightBri = clampi(s.nightBri, 2, 100);
  s.glow     = clampi(s.glow, 0, 100);
  s.volume   = clampi(s.volume, 0, 100);
  s.fwdSign  = s.fwdSign < 0 ? -1 : 1;
  s.overheatC = clampf(s.overheatC, 40, 80);
  s.engineVib = clampf(s.engineVib, 0.001f, 0.1f);   // ENGINE LEARN can set a smooth idle's low level
  s.engOffVib = s.engOffVib <= 0 ? 0 : clampf(s.engOffVib, 0.0003f, s.engineVib * 0.9f);
  s.idleVib   = s.idleVib <= 0 ? 0 : clampf(s.idleVib, 0.0005f, 0.2f);
  s.quietGyro = clampf(s.quietGyro, 0.3f, 3.0f);
  s.quietRoad = clampf(s.quietRoad, 0.002f, 0.05f);
  s.idleGyro = clampf(s.idleGyro, 0, 5); s.idleRoad = clampf(s.idleRoad, 0, 0.1f);
  s.cruiseGyro = clampf(s.cruiseGyro, 0, 20); s.cruiseRoad = clampf(s.cruiseRoad, 0, 0.3f);
  s.idleNoiseDb = clampf(s.idleNoiseDb, -100, 0); s.cruiseNoiseDb = clampf(s.cruiseNoiseDb, -100, 0);
  s.screenFlip = s.screenFlip ? 1 : 0;
  s.engineFelt = s.engineFelt ? 1 : 0;
  s.idleBeat = clampf(s.idleBeat, 0, 1); s.idleBpm = clampf(s.idleBpm, 0, 250);
  s.revRatio  = clampf(s.revRatio, 1.3f, 5.0f);
  s.hotOffC   = clampf(s.hotOffC, 50, 85);
  if (s.hotOffC < s.overheatC + 3) s.hotOffC = s.overheatC + 3;
  s.birthdayMonth = clampi(s.birthdayMonth, 0, 12);
  s.birthdayDay   = clampi(s.birthdayDay,   0, 31);
  s.hotC    = clampi(s.hotC, 25, 70);
  s.coldC   = clampi(s.coldC, -30, 15);
  s.showMin = clampi(s.showMin, 5, 480);
  s.shiftRpm  = s.shiftRpm <= 0 ? 0 : clampi(s.shiftRpm, 2000, 7500);
  s.coldCoolC = clampi(s.coldCoolC, 20, 80);
  s.hotCoolC  = clampi(s.hotCoolC, 95, 130);
  s.showL1[sizeof(s.showL1) - 1] = 0;
  s.showL2[sizeof(s.showL2) - 1] = 0;
}

void settingsLoad() {
  Preferences p;
  p.begin("otto", true);
  uint32_t ver = p.getUInt("ver", 0);
  size_t len = p.getBytesLength("set");
  if (ver == SETTINGS_VERSION && len == sizeof(Settings)) {
    p.getBytes("set", &g_set, sizeof(Settings));
  } else if ((ver == 2 && len == V2_SIZE) || (ver == 3 && len == V3_SIZE) || (ver == 4 && len == V4_SIZE) ||
             (ver == 5 && len == V5_SIZE) || (ver == 6 && len == V6_SIZE) ||
             (ver == 7 && len == V7_SIZE)) {
    static uint8_t buf[sizeof(Settings)];
    p.getBytes("set", buf, len);
    g_set = Settings();
    memcpy(&g_set, buf, ver == 2 ? V2_SIZE : ver == 3 ? V3_USED : ver == 4 ? V4_SIZE : ver == 5 ? V5_SIZE : ver == 6 ? V6_SIZE : V7_SIZE);
  } else {
    g_set = Settings();
  }
  p.end();
  sanitize(g_set);
}

void settingsSave() {
  Preferences p;
  p.begin("otto", false);
  p.putUInt("ver", SETTINGS_VERSION);
  p.putBytes("set", &g_set, sizeof(Settings));
  p.end();
}

void settingsReset() {
  bool cal = g_set.calibrated; float x = g_set.upX, y = g_set.upY, z = g_set.upZ; int f = g_set.fwdSign, sf = g_set.screenFlip;
  g_set = Settings();
  g_set.calibrated = cal; g_set.upX = x; g_set.upY = y; g_set.upZ = z; g_set.fwdSign = f; g_set.screenFlip = sf;  // keep mount
  settingsSave();
}

String settingsToJson() {
  JsonDocument d;
  d["brake"] = g_set.brake;  d["accel"] = g_set.accel; d["turn"] = g_set.turn;
  d["bump"] = g_set.bump;    d["v8"] = g_set.v8;
  d["activity"] = g_set.activity; d["sleepMin"] = g_set.sleepMin;
  d["v8On"] = g_set.v8On; d["bobOn"] = g_set.bobOn; d["boredOn"] = g_set.boredOn;
  d["yawnOn"] = g_set.yawnOn; d["greetOn"] = g_set.greetOn;
  d["dayBri"] = g_set.dayBri; d["nightBri"] = g_set.nightBri; d["glow"] = g_set.glow;
  d["volume"] = g_set.volume;
  d["fwdSign"] = g_set.fwdSign; d["calibrated"] = g_set.calibrated;
  d["overheatC"] = g_set.overheatC;
  d["touchWake"] = g_set.touchWake; d["motionWake"] = g_set.motionWake; d["revOn"] = g_set.revOn; d["launchOn"] = g_set.launchOn;
  d["summaryOn"] = g_set.summaryOn; d["treePro"] = g_set.treePro; d["voiceOn"] = g_set.voiceOn;
  d["engineVib"] = g_set.engineVib; d["revRatio"] = g_set.revRatio; d["hotOffC"] = g_set.hotOffC;
  d["holidaysOn"] = g_set.holidaysOn;
  d["birthdayMonth"] = g_set.birthdayMonth; d["birthdayDay"] = g_set.birthdayDay;
  d["upX"] = g_set.upX; d["upY"] = g_set.upY; d["upZ"] = g_set.upZ;   // mount calibration (so get/cfg restores it)
  d["guardOn"] = g_set.guardOn; d["breakOn"] = g_set.breakOn; d["cabinOn"] = g_set.cabinOn;
  d["milestoneOn"] = g_set.milestoneOn; d["hotC"] = g_set.hotC; d["coldC"] = g_set.coldC;
  d["showMin"] = g_set.showMin; d["showL1"] = g_set.showL1; d["showL2"] = g_set.showL2;
  d["obdOn"] = g_set.obdOn; d["shiftRpm"] = g_set.shiftRpm; d["coldCoolC"] = g_set.coldCoolC; d["hotCoolC"] = g_set.hotCoolC;
  d["wirelessOn"] = g_set.wirelessOn; d["driveLogOn"] = g_set.driveLogOn;
  d["engOffVib"] = g_set.engOffVib; d["idleVib"] = g_set.idleVib;
  d["quietGyro"] = g_set.quietGyro; d["quietRoad"] = g_set.quietRoad;
  d["idleGyro"] = g_set.idleGyro; d["idleRoad"] = g_set.idleRoad;
  d["cruiseGyro"] = g_set.cruiseGyro; d["cruiseRoad"] = g_set.cruiseRoad;
  d["idleNoiseDb"] = g_set.idleNoiseDb; d["idleBeat"] = g_set.idleBeat; d["idleBpm"] = g_set.idleBpm;
  d["cruiseNoiseDb"] = g_set.cruiseNoiseDb; d["screenFlip"] = g_set.screenFlip;
  d["engineFelt"] = g_set.engineFelt;
  String out;
  serializeJson(d, out);
  return out;
}

bool settingsApplyJson(const char* json) {
  JsonDocument d;
  if (deserializeJson(d, json)) return false;
  Settings s = g_set;
#define F(k) if (d[#k].is<float>()) s.k = d[#k].as<float>();
#define I(k) if (d[#k].is<int>() || d[#k].is<float>()) s.k = (int)d[#k].as<float>();
#define B(k) if (d[#k].is<bool>()) s.k = d[#k].as<bool>(); else if (d[#k].is<int>()) s.k = d[#k].as<int>() != 0;
  F(brake) F(accel) F(turn) F(bump) F(v8)
  I(activity) I(sleepMin) I(dayBri) I(nightBri) I(glow) I(volume) I(fwdSign)
  B(v8On) B(bobOn) B(boredOn) B(yawnOn) B(greetOn)
  F(overheatC) F(engineVib) F(revRatio) F(hotOffC)
  B(touchWake) B(motionWake) B(revOn) B(launchOn) B(summaryOn) B(treePro) B(voiceOn)
  B(holidaysOn)
  I(birthdayMonth) I(birthdayDay)
  F(upX) F(upY) F(upZ)
  B(guardOn) B(breakOn) B(cabinOn) B(milestoneOn)
  I(hotC) I(coldC) I(showMin)
  B(obdOn) I(shiftRpm) I(coldCoolC) I(hotCoolC)
  B(wirelessOn) B(driveLogOn) F(engOffVib) F(idleVib) F(quietGyro) F(quietRoad)
  F(idleGyro) F(idleRoad) F(cruiseGyro) F(cruiseRoad) F(idleNoiseDb) F(idleBeat) F(idleBpm) F(cruiseNoiseDb) I(screenFlip) I(engineFelt)
  if (d["showL1"].is<const char*>()) strlcpy(s.showL1, d["showL1"].as<const char*>(), sizeof(s.showL1));
  if (d["showL2"].is<const char*>()) strlcpy(s.showL2, d["showL2"].as<const char*>(), sizeof(s.showL2));
#undef F
#undef I
#undef B
  sanitize(s);
  g_set = s;
  settingsSave();
  return true;
}
