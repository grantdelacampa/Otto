#include "pwrtest.h"
#include "board.h"
#include "config.h"
#include "power.h"
#include "settings.h"
#include "ble.h"
#include "voice.h"
#include "audio.h"
#include <LittleFS.h>

int g_ptBri = -1;
int g_ptFrameMs = 0;

// A step: what to run, then settle (the backlight eases, voice takes a moment to start / stop),
// then average the battery over the measure window. Baseline "base" sits between every test step,
// so slow drift of the cell cancels out.
// Round 1 (Nov 19 2026, base = 90 % / 30 fps / 240 MHz / voice / slow adv; mV less sag than base):
//   bri0 +5.4, bri35 +3.2, bri60 +1.5, fps15 +1.1, fps10 +2.6, cpu160 +0.4, advoff -0.1,
//   advfast -0.3, voiceoff +1.8, voiceoff+80 MHz +2.3, floor +10.7, floor + no BLE stack +12.1.
//   -> backlight ~half of what can be saved, frame rate ~1/4, voice ~1/6; CPU clock and
//   advertising too small to see. ~10 mV for everything: the cell's resistance is low (~0.05 ohm).
// Round 2 (this list): the new defaults (12 fps calm faces, speaker clock off between sounds,
// 30 s advertising) as base; each step puts one old behaviour back, plus the owner's brightnesses.
// frameMs 0 = normal; adv -1 = normal (the 30 s window).
struct Step { const char* name; int bri; int frameMs; int mhz; bool voice; int adv; bool bleOff; bool spk; };
static const Step BASE = {"base", 90, 0, 240, true, -1, false, false};
static const Step kSteps[] = {
  BASE,
  {"spkclock", 90, 0, 240, true, -1, false, true}, BASE,
  {"fps30",    90, 33, 240, true, -1, false, false}, BASE,
  {"bri75",    75, 0, 240, true, -1, false, false}, BASE,
  {"bri10",    10, 0, 240, true, -1, false, false}, BASE,
  {"voiceoff", 90, 0, 240, false, -1, false, false}, BASE,
};
static const int NSTEPS = sizeof(kSteps) / sizeof(kSteps[0]);
static const uint32_t SETTLE_MS = 8000, MEASURE_MS = 12000;
static const char* FILE_PATH = "/pwrtest.csv";

static bool s_on = false;
static uint32_t s_startAt = 0, s_stepAt = 0, s_lastSample = 0;
static int s_step = -1;
static double s_sum = 0;
static uint32_t s_n = 0;
static bool s_voiceWas = true;
static uint32_t s_offAt = 0;        // when the unplug was seen

bool pwrtestActive() { return s_on; }

static void apply(const Step& st) {
  g_ptBri = st.bri;
  g_ptFrameMs = st.frameMs;
  if (getCpuFrequencyMhz() != (uint32_t)st.mhz) setCpuFrequencyMhz(st.mhz);
  g_set.voiceOn = st.voice;           // voiceUpdate() follows it (not saved: settingsSave not called)
  if (st.bleOff) bleShutdown();
  else bleTestAdvertising(st.adv);
  audioHoldClock(st.spk);
}

void pwrtestStart(uint32_t delayMs) {
  if (s_on) return;
  s_on = true;
  s_voiceWas = g_set.voiceOn;
  s_startAt = millis() + delayMs;
  s_step = -1;
  s_offAt = 0;
  LittleFS.remove(FILE_PATH);
  Serial.printf("pwrtest: unplug USB now; starting in %u s, ~%u min, then Otto restarts\n",
                (unsigned)(delayMs / 1000), (unsigned)(NSTEPS * (SETTLE_MS + MEASURE_MS) / 60000 + 1));
}

void pwrtestStop() {
  if (!s_on) return;
  s_on = false;
  g_ptBri = -1; g_ptFrameMs = 0;
  setCpuFrequencyMhz(240);
  g_set.voiceOn = s_voiceWas;
  bleTestAdvertising(-1);
  audioHoldClock(false);
  Serial.println("pwrtest: stopped");
}

static void writeLine(const char* l) {
  File f = LittleFS.open(FILE_PATH, FILE_APPEND);
  if (f) { f.println(l); f.close(); }
}

void pwrtestUpdate(uint32_t now) {
  if (!s_on) return;
  if (s_step < 0) {
    if ((int32_t)(now - s_startAt) < 0) return;
    // Still on USB: the ADC shows the charger, not the cell. Wait up to 30 minutes for the unplug,
    // then 15 s more so the step to the cell's own voltage has settled. The USB host's frames
    // (Serial.isPlugged) are the sure sign: powerCharging() misses a plug-in at boot when the cell
    // is nearly full (no step to see).
    if (Serial.isPlugged() || powerCharging()) {
      s_offAt = 0;
      if ((int32_t)(now - s_startAt) > 1800000) { writeLine("# aborted: still on USB"); pwrtestStop(); }
      return;
    }
    if (!s_offAt) { s_offAt = now; Serial.println("pwrtest: on battery, starting in 15 s"); }
    if (now - s_offAt < 15000) return;
    char b[64];
    snprintf(b, sizeof(b), "# start %lu, %.3f V", (unsigned long)rtcNowUtc(), powerVolts());
    writeLine(b);
    s_step = 0; s_stepAt = now; s_sum = 0; s_n = 0;
    apply(kSteps[0]);
    return;
  }
  uint32_t in = now - s_stepAt;
  if (in >= SETTLE_MS && now - s_lastSample >= 10) {
    s_lastSample = now;
    s_sum += analogReadMilliVolts(PIN_BAT_ADC) * 3.0;
    s_n++;
  }
  if (in < SETTLE_MS + MEASURE_MS) return;
  char b[80];
  snprintf(b, sizeof(b), "%d,%s,%.2f,%lu", s_step, kSteps[s_step].name, s_n ? s_sum / s_n : 0.0, (unsigned long)s_n);
  writeLine(b);
  Serial.println(b);
  if (++s_step >= NSTEPS) {
    writeLine("# done");
    Serial.flush();
    delay(100);
    ESP.restart();                     // the cleanest way back to normal (Bluetooth stack is off)
  }
  s_stepAt = now; s_sum = 0; s_n = 0;
  apply(kSteps[s_step]);
}

void pwrtestReport(uint16_t src, void (*reply)(uint16_t, const char*)) {
  File f = LittleFS.open(FILE_PATH, FILE_READ);
  if (!f) { reply(src, "pwrtest: no results (pwrtest start [secs])"); return; }
  struct R { char name[16]; double mv; } r[NSTEPS];
  int n = 0;
  while (f.available()) {
    String s = f.readStringUntil('\n');
    s.trim();
    if (!s.length()) continue;
    reply(src, s.c_str());
    int i; char nm[16]; double mv; unsigned long cnt;
    if (s[0] != '#' && sscanf(s.c_str(), "%d,%15[^,],%lf,%lu", &i, nm, &mv, &cnt) == 4 && i == n && n < NSTEPS) {
      strlcpy(r[n].name, nm, sizeof(r[n].name)); r[n].mv = mv; n++;
    }
  }
  f.close();
  // each test step vs the mean of the baselines around it (+ = it saves this many mV of sag)
  char b[96];
  double lastBase = 0;
  for (int i = 0; i < n; i++) {
    if (!strcmp(r[i].name, "base")) { lastBase = r[i].mv; continue; }
    double ref = lastBase;
    if (i + 1 < n && !strcmp(r[i + 1].name, "base")) ref = (lastBase + r[i + 1].mv) / 2;
    snprintf(b, sizeof(b), "pwrtest: %-12s %+6.1f mV vs base", r[i].name, r[i].mv - ref);
    reply(src, b);
  }
}
