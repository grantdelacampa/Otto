#include "menu.h"
#include "config.h"
#include "display.h"
#include "face.h"
#include "settings.h"
#include "brain.h"
#include "audio.h"
#include "motion.h"
#include "power.h"
#include "board.h"
#include "ble.h"
#include "launch.h"
#include "extras.h"
#include "holidays.h"
#include "obd.h"
#include "wifitune.h"
#include <time.h>

enum class Kind : uint8_t { Value, Toggle, Action, BdayPicker };
enum class View : uint8_t { List, Edit, ConfirmCal, Info, BdayMonth, BdayDay };
enum class Act : uint8_t { None, Launch, Recal, Info, Exit, Birthday, Show, Codes, Wifi };

struct Item {
  const char* name;
  Kind kind;
  int* ival;
  bool* bval;
  int lo, hi, step;
  Act act;
};

static Item s_items[] = {
  {"BRIGHTNESS",   Kind::Value,  &g_set.dayBri,   nullptr, 10, 100, 5, Act::None},
  {"NIGHT BRIGHT", Kind::Value,  &g_set.nightBri, nullptr, 5, 100, 5, Act::None},
  {"VOLUME",       Kind::Value,  &g_set.volume,   nullptr, 0, 100, 10, Act::None},
  {"GREETINGS",    Kind::Toggle, nullptr, &g_set.greetOn, 0, 0, 0, Act::None},
  {"REVS",         Kind::Toggle, nullptr, &g_set.revOn,   0, 0, 0, Act::None},
  {"V8 MODE",      Kind::Toggle, nullptr, &g_set.v8On,    0, 0, 0, Act::None},
  {"MUSIC BOB",    Kind::Toggle, nullptr, &g_set.bobOn,   0, 0, 0, Act::None},
  {"BORED",        Kind::Toggle, nullptr, &g_set.boredOn, 0, 0, 0, Act::None},
  {"YAWNS",        Kind::Toggle, nullptr, &g_set.yawnOn,  0, 0, 0, Act::None},
  {"HOLIDAYS",     Kind::Toggle, nullptr, &g_set.holidaysOn, 0, 0, 0, Act::None},
  {"MOTION WAKE",  Kind::Toggle, nullptr, &g_set.motionWake, 0, 0, 0, Act::None},
  {"BIRTHDAY",     Kind::BdayPicker, nullptr, nullptr, 0, 0, 0, Act::Birthday},
  {"VOICE",        Kind::Toggle, nullptr, &g_set.voiceOn, 0, 0, 0, Act::None},
  {"PRO TREE",     Kind::Toggle, nullptr, &g_set.treePro, 0, 0, 0, Act::None},
  {"LAUNCH MODE",  Kind::Action, nullptr, nullptr, 0, 0, 0, Act::Launch},
  {"CAR SHOW",     Kind::Action, nullptr, nullptr, 0, 0, 0, Act::Show},
  {"GUARD",        Kind::Toggle, nullptr, &g_set.guardOn, 0, 0, 0, Act::None},
  {"OBD ADAPTER",  Kind::Toggle, nullptr, &g_set.obdOn,   0, 0, 0, Act::None},
  {"ENGINE CODES", Kind::Action, nullptr, nullptr, 0, 0, 0, Act::Codes},
  {"WI-FI TUNING", Kind::Action, nullptr, nullptr, 0, 0, 0, Act::Wifi},
  {"RECALIBRATE",  Kind::Action, nullptr, nullptr, 0, 0, 0, Act::Recal},
  {"INFO",         Kind::Action, nullptr, nullptr, 0, 0, 0, Act::Info},
  {"EXIT",         Kind::Action, nullptr, nullptr, 0, 0, 0, Act::Exit},
};
static const int N = sizeof(s_items) / sizeof(s_items[0]);

static bool s_open = false;
static View s_view = View::List;
static int s_sel = 0;
static uint32_t s_lastTouch = 0;
static bool s_dirty = false;
static int s_bdayMon = 0, s_bdayDay = 0;  // temp state for birthday picker

bool menuIsOpen() { return s_open; }

void menuOpen() {
  s_open = true;
  s_view = View::List;
  s_sel = 0;
  s_lastTouch = millis();
  s_dirty = false;
}

void menuClose(bool save) {
  if (!s_open) return;
  s_open = false;
  if (save && s_dirty) settingsSave();
  brainEvent(Expr::Saved, 1300);
}

void menuTouched(uint32_t now) { s_lastTouch = now; }

static const char* kMonths[] = {"", "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                     "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

static void valueText(const Item& it, char* b, size_t n) {
  if (it.kind == Kind::Value) snprintf(b, n, "%d%%", *it.ival);
  else if (it.kind == Kind::Toggle) snprintf(b, n, "%s", *it.bval ? "ON" : "OFF");
  else if (it.kind == Kind::BdayPicker) {
    if (g_set.birthdayMonth > 0 && g_set.birthdayDay > 0)
      snprintf(b, n, "%s %d", kMonths[g_set.birthdayMonth], g_set.birthdayDay);
    else snprintf(b, n, "NOT SET");
  } else b[0] = 0;
}

void menuTap(float, float) {
  s_lastTouch = millis();
  switch (s_view) {
    case View::List: {
      Item& it = s_items[s_sel];
      if (it.kind == Kind::Value) { s_view = View::Edit; audioChirp(1000, 1200, 50); }
      else if (it.kind == Kind::Toggle) {
        *it.bval = !*it.bval; s_dirty = true;
        audioChirp(*it.bval ? 900 : 1200, *it.bval ? 1300 : 800, 60);
      } else if (it.kind == Kind::BdayPicker) {
        // Start the birthday month picker
        s_bdayMon = g_set.birthdayMonth > 0 ? g_set.birthdayMonth : 1;
        s_bdayDay = g_set.birthdayDay   > 0 ? g_set.birthdayDay   : 1;
        s_view = View::BdayMonth;
        audioChirp(1000, 1200, 50);
      } else {
        switch (it.act) {
          case Act::Launch: menuClose(true); launchArm(); break;
          case Act::Show: menuClose(true); extrasShow(true); break;
          case Act::Recal: s_view = View::ConfirmCal; break;
          case Act::Info: s_view = View::Info; break;
          case Act::Codes: menuClose(true); obdShowCodes(); break;
          case Act::Wifi: menuClose(true); if (wifiTuneActive()) wifiTuneStop(); else wifiTuneStart(); break;
          case Act::Exit: menuClose(true); break;
          default: break;
        }
      }
      break;
    }
    case View::Edit: s_view = View::List; audioChirp(1200, 1000, 50); break;
    case View::ConfirmCal: menuClose(true); motionStartCalibration(); break;
    case View::Info: s_view = View::List; break;
    case View::BdayMonth:
      // Tap: confirm month, move to day picker
      s_view = View::BdayDay;
      audioChirp(1000, 1200, 50);
      break;
    case View::BdayDay:
      // Tap: save and return
      g_set.birthdayMonth = s_bdayMon;
      g_set.birthdayDay   = s_bdayDay;
      s_dirty = true;
      s_view = View::List;
      audioChirp(1200, 1000, 50);
      brainEvent(Expr::Saved, 1300);
      break;
  }
}

static int daysInMonth(int mon) {
  static const int kDays[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
  return (mon >= 1 && mon <= 12) ? kDays[mon] : 30;
}

void menuSwipe(float dy) {
  s_lastTouch = millis();
  int steps = max(1, (int)(fabsf(dy) / 40.0f));
  if (s_view == View::List) {
    // swipe up = move down the list
    s_sel += dy < 0 ? steps : -steps;
    s_sel = constrain(s_sel, 0, N - 1);
    audioChirp(1300, 1300, 15);
  } else if (s_view == View::Edit) {
    Item& it = s_items[s_sel];
    int v = *it.ival + (dy < 0 ? 1 : -1) * steps * it.step;   // swipe up = more
    *it.ival = constrain(v, it.lo, it.hi);
    s_dirty = true;
    audioChirp(1000 + *it.ival * 8, 1000 + *it.ival * 8, 25);
  } else if (s_view == View::BdayMonth) {
    s_bdayMon += (dy < 0 ? 1 : -1);
    if (s_bdayMon < 1) s_bdayMon = 12;
    if (s_bdayMon > 12) s_bdayMon = 1;
    if (s_bdayDay > daysInMonth(s_bdayMon)) s_bdayDay = daysInMonth(s_bdayMon);
    audioChirp(1300, 1300, 15);
  } else if (s_view == View::BdayDay) {
    s_bdayDay += (dy < 0 ? 1 : -1);
    int maxD = daysInMonth(s_bdayMon);
    if (s_bdayDay < 1) s_bdayDay = maxD;
    if (s_bdayDay > maxD) s_bdayDay = 1;
    audioChirp(1300, 1300, 15);
  }
}

// Close the menu only when the car is really driving off: ~3 s of sustained
// motion. Finger presses, a shaky hand or a bump are short and drain the
// counter again, so they no longer close the menu (owner's request).
static const float MENU_MOVE_G    = 0.15f;   // g, sustained push (drive-off is ~0.15-0.3 g)
static const float MENU_MOVE_YAW  = 15.0f;   // deg/s, sustained turn
static const uint32_t MENU_MOVE_MS = 3000;   // this much motion closes the menu
static uint32_t s_moveMs = 0, s_lastUpd = 0;

void menuUpdate(uint32_t now) {
  if (!s_open) { s_moveMs = 0; s_lastUpd = now; return; }
  uint32_t dt = now - s_lastUpd;
  s_lastUpd = now;
  if (dt > 200) dt = 200;   // skip gaps (first frame after opening, slow redraws)
  bool moving = g_motion.ok && (fabsf(g_motion.lon) > MENU_MOVE_G || fabsf(g_motion.lat) > MENU_MOVE_G ||
                                fabsf(g_motion.yaw) > MENU_MOVE_YAW);
  if (moving) s_moveMs += dt;
  else s_moveMs = s_moveMs > 2 * dt ? s_moveMs - 2 * dt : 0;   // drains twice as fast as it fills
  if (s_moveMs >= MENU_MOVE_MS) { menuClose(true); return; }
  if (now - s_lastTouch > 15000) menuClose(true);
}

// ----------------------------------------------------------------- draw ---
static void drawList(uint32_t now) {
  (void)now;
  faceText("CONFIG", 120, 40, 2, 14, COL_YELLOW);
  char b[24], line[40];
  // row above
  if (s_sel > 0) {
    const Item& it = s_items[s_sel - 1];
    valueText(it, b, sizeof(b));
    snprintf(line, sizeof(line), b[0] ? "%s - %s" : "%s", it.name, b);
    faceText(line, 120, 80, 2, 14, COL_ICE_DIM);
  }
  // selected row
  const Item& sel = s_items[s_sel];
  canvas.fillRect(34 * LCD_W / 240, 96 * LCD_W / 240, 172 * LCD_W / 240, 40 * LCD_W / 240, rgb565(18, 34, 38));
  for (int i = 0; i < 3; i++)
    canvas.drawRect(34 * LCD_W / 240 + i, 96 * LCD_W / 240 + i, 172 * LCD_W / 240 - 2 * i, 40 * LCD_W / 240 - 2 * i, COL_ICE);
  faceText(sel.name, 46, 116, 4, 14, COL_ICE, Align::Left);
  if (sel.kind == Kind::Toggle) {
    float s = LCD_W / 240.0f;
    bool on = *sel.bval;
    if (on) canvas.fillRoundRect(150 * s, 105 * s, 46 * s, 22 * s, 11 * s, COL_YELLOW);
    else for (int i = 0; i < 2; i++) canvas.drawRoundRect(150 * s + i, 105 * s + i, 46 * s - 2 * i, 22 * s - 2 * i, 11 * s, COL_GRAY);
    canvas.fillSmoothCircle((on ? 185 : 161) * s, 116 * s, 8 * s, on ? COL_BG : COL_GRAY);
  } else if (sel.kind == Kind::Value) {
    valueText(sel, b, sizeof(b));
    faceText(b, 194, 117, 4, 18, COL_PINK, Align::Right);
  }
  // rows below
  for (int k = 1; k <= 2; k++) {
    if (s_sel + k >= N) break;
    const Item& it = s_items[s_sel + k];
    valueText(it, b, sizeof(b));
    snprintf(line, sizeof(line), b[0] ? "%s - %s" : "%s", it.name, b);
    faceText(line, 120, k == 1 ? 160 : 184, 2, k == 1 ? 14 : 12, k == 1 ? COL_GRAY : COL_ICE_DIM);
  }
  // scroll position on the right edge
  float s = LCD_W / 240.0f;
  float a = -40 + 80.0f * s_sel / (N - 1);
  canvas.fillArc(120 * s, 120 * s, 108.5f * s, 111.5f * s, -40, 40, COL_GRID);
  canvas.fillArc(120 * s, 120 * s, 108.5f * s, 111.5f * s, a - 4, a + 4, COL_ICE);
  faceText("SWIPE - TAP - EXIT TO CLOSE", 120, 214, 2, 11, COL_GRAY);
}

static void drawEdit() {
  const Item& it = s_items[s_sel];
  float s = LCD_W / 240.0f;
  float p = (float)(*it.ival - it.lo) / (it.hi - it.lo);
  canvas.fillArc(120 * s, 120 * s, 101 * s, 111 * s, 135, 405, COL_GRID);
  if (p > 0.005f) canvas.fillArc(120 * s, 120 * s, 101 * s, 111 * s, 135, 135 + 270 * p, COL_ICE);
  char b[12];
  snprintf(b, sizeof(b), "%d%%", *it.ival);
  faceText(it.name, 120, 92, 2, 14, COL_ICE);
  faceText(b, 120, 140, 4, 42, COL_ICE);
  faceText("SWIPE UP/DOWN - TAP = DONE", 120, 178, 2, 12, COL_GRAY);
}

static void drawInfo() {
  char b[32];
  faceText("INFO", 120, 48, 2, 14, COL_YELLOW);
  struct Row { const char* k; char v[24]; } rows[6];
  snprintf(rows[0].v, 24, "%d%% %.2fV", powerPct(), powerVolts());       rows[0].k = "BAT";
  if (shtc3Ok()) {                                                        // cabin sensor
    snprintf(rows[1].v, 24, "%dC %d%%", (int)lroundf(shtc3TempC()), (int)lroundf(shtc3Humidity()));
    rows[1].k = "CABIN";
  } else {
    snprintf(rows[1].v, 24, "%dC", (int)g_motion.tempC);
    rows[1].k = "TEMP";
  }
  snprintf(rows[2].v, 24, "%s", bleAnyConnected() ? "CONNECTED" : "--");    rows[2].k = "TUNER";
  if (rtcValid()) {
    time_t now = time(nullptr) + tzOffsetMin() * 60;
    struct tm t;
    gmtime_r(&now, &t);
    snprintf(rows[3].v, 24, "%02d:%02d", t.tm_hour, t.tm_min);
  } else snprintf(rows[3].v, 24, "--:--");
  rows[3].k = "CLOCK";
  float best = launchBest(), bestRt = launchBestRt();
  if (best > 0) { snprintf(rows[4].v, 24, "%.1f S", best); rows[4].k = "BEST 0-60"; }
  else {                                     // no speed source on Otto: best reaction time instead
    if (bestRt > 0) snprintf(rows[4].v, 24, "%.3f S", bestRt); else snprintf(rows[4].v, 24, "--");
    rows[4].k = "BEST R/T";
  }
  snprintf(rows[5].v, 24, "%s", FW_VERSION);                              rows[5].k = "FW";
  float s = LCD_W / 240.0f;
  for (int i = 0; i < 6; i++) {
    float y = 76 + i * 24;
    faceText(rows[i].k, 46, y, 2, 14, COL_GRAY, Align::Left);
    faceText(rows[i].v, 194, y, 2, 14, COL_ICE, Align::Right);
    if (i < 5) canvas.drawFastHLine(46 * s, (y + 12) * s, 148 * s, COL_GRID);
  }
  (void)b;
  faceText("TAP = BACK", 120, 222, 2, 11, COL_GRAY);
}

// Clock-face picker: draws a ring of 12 or 31 tick marks, highlights the
// selected value, shows the value large in the centre.
static void drawClockPicker(uint32_t now, int val, int maxVal, const char* title,
                             const char* sub, bool useNames = false) {
  (void)now;
  float s = LCD_W / 240.0f;
  // ring of ticks
  float R = 101 * s, r0 = 95 * s, r1 = 108 * s;
  int ticks = (maxVal <= 12) ? 12 : 31;
  for (int i = 1; i <= ticks; i++) {
    float a = ((i - 1) / (float)ticks - 0.25f) * 2 * PI;
    bool hi = (i == val);
    uint16_t c = hi ? COL_ICE : COL_GRID;
    float ir = hi ? r0 - 4 * s : r0;
    float or_ = hi ? r1 + 4 * s : r1;
    canvas.drawWideLine(120 * s + ir * cosf(a), 120 * s + ir * sinf(a),
                        120 * s + or_ * cosf(a), 120 * s + or_ * sinf(a),
                        hi ? 5 : 2, c);
  }
  faceText(title, 120, 54, 2, 14, COL_YELLOW);
  // centre: value
  char big[8];
  if (useNames && val >= 1 && val <= 12) snprintf(big, sizeof(big), "%s", kMonths[val]);
  else snprintf(big, sizeof(big), "%d", val);
  faceText(big, 120, 128, 4, 42, COL_ICE);
  faceText(sub, 120, 196, 2, 12, COL_GRAY);
}

static void drawBdayMonth(uint32_t now) {
  drawClockPicker(now, s_bdayMon, 12, "BIRTHDAY", "SWIPE = MONTH   TAP = NEXT", true);
}

static void drawBdayDay(uint32_t now) {
  char title[16];
  snprintf(title, sizeof(title), "%s", kMonths[s_bdayMon]);
  drawClockPicker(now, s_bdayDay, daysInMonth(s_bdayMon), title, "SWIPE = DAY   TAP = SAVE", false);
}

static void drawConfirm(uint32_t now) {
  FaceState f;
  f.ov = 0; f.dashed = true; f.rbright = 0.6f; f.fcy = 108; f.eyeR = 8;
  FaceText tx;
  faceRender(f, tx, 0, 1, 0, now / 1000.0f);
  faceText("RECALIBRATE?", 120, 178, 4, 15, COL_ICE);
  float pulse = 0.35f + 0.65f * (0.5f + 0.5f * cosf(now / 1000.0f * 2 * PI / 1.2f));
  faceText("TAP TO START", 120, 204, 2, 15, mix565(C_BG, C_YELLOW, pulse));
}

void menuRender(uint32_t now) {
  if (s_view == View::ConfirmCal) { drawConfirm(now); return; }
  canvas.fillScreen(COL_BG);
  switch (s_view) {
    case View::List:      drawList(now); break;
    case View::Edit:      drawEdit(); break;
    case View::Info:      drawInfo(); break;
    case View::BdayMonth: drawBdayMonth(now); break;
    case View::BdayDay:   drawBdayDay(now); break;
    default: break;
  }
}
