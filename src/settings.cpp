#include "settings.h"
#include <Preferences.h>
#include <ArduinoJson.h>

Settings g_set;
static const uint32_t SETTINGS_VERSION = 2;   // Otto starts its own numbering (own NVS namespace)

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
  s.engineVib = clampf(s.engineVib, 0.002f, 0.1f);
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
  if (p.getUInt("ver", 0) == SETTINGS_VERSION && p.getBytesLength("set") == sizeof(Settings)) {
    p.getBytes("set", &g_set, sizeof(Settings));
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
  bool cal = g_set.calibrated; float x = g_set.upX, y = g_set.upY, z = g_set.upZ; int f = g_set.fwdSign;
  g_set = Settings();
  g_set.calibrated = cal; g_set.upX = x; g_set.upY = y; g_set.upZ = z; g_set.fwdSign = f;  // keep mount
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
