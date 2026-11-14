#include "power.h"
#include "board.h"
#include <LittleFS.h>

static float s_v = 3.9f;
static uint32_t s_last = 0, s_winStart = 0;
static float s_winSum = 0;
static int s_winN = 0;
static float s_hist[4] = {0, 0, 0, 0};  // 30 s averages, last 2 minutes
static int s_histN = 0;
static bool s_chg = false, s_full = false;
static float s_fake = 0;
static float readV() { return s_fake > 0 ? s_fake : boardBatteryVolts(); }
void powerFake(float v) { s_fake = v; if (v > 0) s_v = v; s_histN = 0; }
bool powerFull() { return s_full; }

void powerInit() {
  float v = 0;
  for (int i = 0; i < 8; i++) v += readV();
  s_v = v / 8;
}

void powerUpdate(uint32_t now) {
  if (now - s_last < 200) return;
  s_last = now;
  s_v += (readV() - s_v) * 0.05f;

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
    else if (falling || avg < 4.05f) s_chg = false;
    // full: on the charger, at the top, and no longer rising over the last 2 minutes
    bool flat = s_histN >= 4 && fabsf(s_hist[0] - s_hist[3]) < 0.006f;
    if (s_chg && avg > 4.17f && flat) s_full = true;
    if (!s_chg || avg < 4.10f) s_full = false;
  }
}

float powerVolts() { return s_v; }

int powerPct() {
  static const float v[] = {3.30f, 3.50f, 3.60f, 3.65f, 3.70f, 3.75f, 3.80f, 3.90f, 4.00f, 4.10f, 4.20f};
  static const int p[] = {0, 5, 12, 20, 30, 40, 50, 65, 80, 90, 100};
  if (s_v <= v[0]) return 0;
  for (int i = 1; i < 11; i++) {
    if (s_v <= v[i]) return p[i - 1] + (int)((s_v - v[i - 1]) / (v[i] - v[i - 1]) * (p[i] - p[i - 1]));
  }
  return 100;
}

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
  if (capMah <= 0) capMah = 3000;
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
      bool wakeLine = !strcmp(ev, "wake");
      if (pt && t > pt && mv < 4150) {           // skip USB/charging (cell full) and clock jumps
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
  snprintf(line, sizeof(line), "batlog: asleep %.1f h, %.1f mV/h (~%.1f mA)", sleepH, sleepH > 0 ? sleepMv / sleepH : 0, ma(sleepMv, sleepH));
  reply(src, line);
  snprintf(line, sizeof(line), "batlog: mA figures assume a %.0f mAh cell (batlog <mAh> to change); rough", capMah);
  reply(src, line);
}
