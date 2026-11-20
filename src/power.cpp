#include "power.h"
#include "board.h"
#include <LittleFS.h>

static float s_v = 3.9f;        // slow filter (~4 s): what percent and the trend rules use
static float s_fast = 3.9f;     // quick filter (~1 s): plug / unplug edges
static float s_ago[16];         // s_fast over the last ~3.2 s (one per 200 ms update)
static int s_agoI = 0;
static bool s_agoFull = false;
static uint32_t s_edgeQuiet = 0;
static uint32_t s_last = 0, s_winStart = 0;
static float s_winSum = 0;
static int s_winN = 0;
static float s_hist[4] = {0, 0, 0, 0};  // 30 s averages, last 2 minutes
static int s_histN = 0;
static bool s_chg = false, s_full = false;
static int s_hold = -1;         // percent from just before the charger was plugged in (-1 = unknown)
static float s_fake = 0;
// Survive deep sleep (a wake is a reboot): plugged in while he slept still shows as charging.
RTC_DATA_ATTR static uint32_t s_rtcMagic = 0;
RTC_DATA_ATTR static float s_rtcV = 0;
RTC_DATA_ATTR static bool s_rtcChg = false;
RTC_DATA_ATTR static int s_rtcHold = -1;
static const uint32_t RTC_MAGIC = 0x0BA77E21;
// On the charger the ADC reads the charger's ~4.13 V at once, whatever the cell holds (a 4 % cell
// jumped 3.49 -> 4.13 V), and it never gets above 4.15 V; so plugging in / unplugging shows up as
// a step, and percent from voltage means nothing until he's unplugged again.
static const float EDGE_V = 0.08f;
static float readV() { return s_fake > 0 ? s_fake : boardBatteryVolts(); }
void powerFake(float v) { s_fake = v; s_histN = 0; }   // the step itself acts as plug / unplug
bool powerFull() { return s_full; }

static int pctFromV(float x) {
  static const float v[] = {3.30f, 3.50f, 3.60f, 3.65f, 3.70f, 3.75f, 3.80f, 3.90f, 4.00f, 4.10f, 4.20f};
  static const int p[] = {0, 5, 12, 20, 30, 40, 50, 65, 80, 90, 100};
  if (x <= v[0]) return 0;
  for (int i = 1; i < 11; i++) {
    if (x <= v[i]) return p[i - 1] + (int)((x - v[i - 1]) / (v[i] - v[i - 1]) * (p[i] - p[i - 1]));
  }
  return 100;
}

static void saveRtc() { s_rtcMagic = RTC_MAGIC; s_rtcV = s_v; s_rtcChg = s_chg; s_rtcHold = s_hold; }

void powerInit() {
  float v = 0;
  for (int i = 0; i < 8; i++) v += readV();
  s_v = s_fast = v / 8;
  if (s_rtcMagic == RTC_MAGIC && s_rtcV > 2.5f) {
    float d = s_v - s_rtcV;
    if (d > EDGE_V) { s_chg = true; s_hold = pctFromV(s_rtcV); }          // plugged in while asleep
    else if (s_rtcChg && d > -EDGE_V) { s_chg = true; s_hold = s_rtcHold; } // still on the charger
  }
  saveRtc();
}

void powerUpdate(uint32_t now) {
  if (now - s_last < 200) return;
  s_last = now;
  float r = readV();
  s_fast += (r - s_fast) * 0.2f;
  s_v += (r - s_v) * 0.05f;

  // Edge = quick reading vs the quick reading ~3 s ago. (Comparing against the slow filter only
  // ever saw half the step - it chases the quick one - so a cell above ~3.97 V, whose step to the
  // charger's 4.13 V is < 0.16 V, never registered a plug-in.)
  const int N = sizeof(s_ago) / sizeof(s_ago[0]);
  if (now < s_edgeQuiet) s_agoFull = false;              // after an edge: restart from the settled value
  if (!s_agoFull) { for (int i = 0; i < N; i++) s_ago[i] = s_fast; s_agoFull = true; }
  float before = s_ago[s_agoI];                          // oldest = ~3.2 s ago
  s_ago[s_agoI] = s_fast; s_agoI = (s_agoI + 1) % N;
  float step = s_fast - before;
  if (now >= s_edgeQuiet && fabsf(step) > EDGE_V) {
    if (step > 0) {                                      // plugged in
      if (!s_chg) { s_chg = true; s_hold = pctFromV(before); }
      Serial.printf("power: plugged in (%.2f -> %.2f V), holding %d%%\n", before, s_fast, s_hold);
    } else {                                             // unplugged (or a big load step)
      Serial.printf("power: unplugged (%.2f -> %.2f V)\n", before, s_fast);
      s_chg = false; s_full = false; s_hold = -1;
    }
    s_v = s_fast; s_histN = 0;
    s_edgeQuiet = now + 4000;                            // let the quick filter settle, then re-arm
  }

  // A USB host (computer) is the sure sign of a charger: its frames reach the native USB port.
  // The step above misses a plug-in at boot with a nearly full cell (4.12 V cell vs the charger's
  // 4.13 V: nothing to see), so Otto never knew. A plain charger (car, wall) sends no frames:
  // that one is still caught by the step. (Nov 19 2026)
  static bool s_host = false;
  bool host = Serial.isPlugged();
  if (host && !s_chg) { s_chg = true; s_hold = -1; }              // percent: unknown -> from voltage
  if (s_host && !host && s_chg) { s_chg = false; s_full = false; s_hold = -1; s_v = s_fast; s_histN = 0; }
  s_host = host;

  s_winSum += s_v; s_winN++;
  if (now - s_winStart >= 30000) {
    float avg = s_winSum / s_winN;
    for (int i = 3; i > 0; i--) s_hist[i] = s_hist[i - 1];
    s_hist[0] = avg;
    if (s_histN < 4) s_histN++;
    s_winSum = 0; s_winN = 0; s_winStart = now;
    bool rising = s_histN >= 4 && (s_hist[0] - s_hist[3]) > 0.02f;
    bool falling = s_histN >= 2 && (s_hist[0] - s_hist[1]) < -0.01f;
    if (avg > 4.15f || rising) s_chg = true;
    else if (!s_host && (falling || avg < 4.05f)) { s_chg = false; s_hold = -1; }
    // full: on the charger, at the top, and no longer rising over the last 2 minutes
    bool flat = s_histN >= 4 && fabsf(s_hist[0] - s_hist[3]) < 0.006f;
    if (s_chg && avg > 4.17f && flat) s_full = true;
    if (!s_chg || avg < 4.10f) s_full = false;
  }
  saveRtc();
}

float powerVolts() { return s_v; }

// While charging the voltage only shows the charger, so keep the last real reading until unplugged.
int powerPct() { return s_chg && s_hold >= 0 ? s_hold : pctFromV(s_v); }

bool powerCharging() { return s_chg; }

// ------------------------------------------------------------ battery log --
static const char* LOG = "/batlog.csv";
static const char* LOG_OLD = "/batlog.old";

void powerLog(const char* event) {
  File f = LittleFS.open(LOG, FILE_APPEND);
  if (!f) return;
  if (f.size() > 48 * 1024) {                     // keep it small: roll over to one old file
    f.close();
    LittleFS.remove(LOG_OLD);
    LittleFS.rename(LOG, LOG_OLD);
    f = LittleFS.open(LOG, FILE_APPEND);
    if (!f) return;
  }
  float v = 0;
  for (int i = 0; i < 8; i++) v += readV();   // fresh reading, not the slow filter
  v /= 8;
  f.printf("%lu,%d,%d,%s\n", (unsigned long)rtcNowUtc(), (int)(v * 1000 + 0.5f), powerPct(), event);
  f.close();
}

void powerLogClear() { LittleFS.remove(LOG); LittleFS.remove(LOG_OLD); }

void powerLogDump(uint16_t src, void (*reply)(uint16_t, const char*), float capMah) {
  if (capMah <= 0) capMah = 500;     // owner's cell (Nov 19 2026)
  char line[96];
  // Drain rates: awake = between consecutive lines while awake; asleep = from "sleep" to the next "wake".
  double awakeH = 0, awakeMv = 0, sleepH = 0, sleepMv = 0;
  unsigned long pt = 0; int pmv = 0; char pev[12] = "";
  int n = 0;
  for (const char* path : {LOG_OLD, LOG}) {
    if (!LittleFS.exists(path)) continue;
    File f = LittleFS.open(path, FILE_READ);
    if (!f) continue;
    while (f.available()) {
      String s = f.readStringUntil('\n');
      s.trim();
      if (!s.length()) continue;
      reply(src, s.c_str());
      n++;
      unsigned long t = 0; int mv = 0, pct = 0; char ev[12] = "";
      if (sscanf(s.c_str(), "%lu,%d,%d,%11s", &t, &mv, &pct, ev) != 4) continue;
      bool wakeLine = !strncmp(ev, "wake", 4);   // wake, wake-peek, wake-pwr, wake-boot
      // skip USB/charging (cell full or a jump up = plugged in) and clock jumps
      if (pt && t > pt && mv < 4150 && mv - pmv < 100) {
        double h = (t - pt) / 3600.0;
        if (!strcmp(pev, "sleep") && wakeLine) { sleepH += h; sleepMv += pmv - mv; }
        else if (!strcmp(ev, "awake") || !strcmp(ev, "sleep") || !strcmp(ev, "off")) {
          if (strcmp(pev, "sleep") && strcmp(pev, "off")) { awakeH += h; awakeMv += pmv - mv; }
        }
      }
      pt = t; pmv = mv; strncpy(pev, ev, sizeof(pev) - 1);
    }
    f.close();
  }
  if (!n) { reply(src, "batlog: empty"); return; }
  // ~0.9 V from full to empty, roughly linear in the middle of the curve -> mA estimate
  auto ma = [&](double mv, double h) { return h > 0 ? mv / h / 900.0 * capMah : 0; };
  snprintf(line, sizeof(line), "batlog: awake %.1f h, %.0f mV/h (~%.0f mA)", awakeH, awakeH > 0 ? awakeMv / awakeH : 0, ma(awakeMv, awakeH));
  reply(src, line);
  // the cell recovers a little while the load is off, so short naps can read as a gain
  if (sleepH > 0 && sleepMv <= 0) snprintf(line, sizeof(line), "batlog: asleep %.1f h, drain too small to measure", sleepH);
  else snprintf(line, sizeof(line), "batlog: asleep %.1f h, %.1f mV/h (~%.1f mA)", sleepH, sleepH > 0 ? sleepMv / sleepH : 0, ma(sleepMv, sleepH));
  reply(src, line);
  snprintf(line, sizeof(line), "batlog: mA figures assume a %.0f mAh cell (batlog <mAh> to change); rough", capMah);
  reply(src, line);
}
