#include "holidays.h"
#include "brain.h"
#include "board.h"
#include "audio.h"
#include "settings.h"
#include "face.h"
#include <time.h>

// ---------------------------------------------------------------- helpers ---

static bool localDate(int& mon, int& mday, int& wday) {
  if (!rtcValid()) return false;
  time_t t = rtcNowUtc() + (time_t)tzOffsetMin() * 60;
  struct tm tm;
  gmtime_r(&t, &tm);
  mon  = tm.tm_mon + 1;   // 1-12
  mday = tm.tm_mday;      // 1-31
  wday = tm.tm_wday;      // 0=Sun
  return true;
}

// 4th Thursday of November
static bool isThanksgiving(int mon, int mday) {
  if (mon != 11) return false;
  // Find day-of-week for Nov 1 — work backwards from mday
  // We need to know what weekday Nov 1 is for this year.
  // localDate() gives us the current wday for mday; use that to find Nov 1.
  // wday for Nov 1 = (wday - (mday - 1) % 7 + 70) % 7
  // But we don't have year — build it from time_t.
  time_t t = rtcNowUtc() + (time_t)tzOffsetMin() * 60;
  struct tm tm;
  gmtime_r(&t, &tm);
  // Find the first Thursday of November
  struct tm nov1 = tm;
  nov1.tm_mon = 10; nov1.tm_mday = 1; nov1.tm_hour = 12; nov1.tm_min = 0; nov1.tm_sec = 0;
  // timegm is not in ESP32 SDK; use mktime (local tz may differ, but we only need the weekday)
  time_t nov1t = mktime(&nov1);  // ok if off by tz hours — just need weekday
  struct tm nov1r;
  localtime_r(&nov1t, &nov1r);
  int nov1_dow = nov1r.tm_wday;  // 0=Sun
  // First Thursday on or after Nov 1
  int first_thu = 1 + (4 - nov1_dow + 7) % 7;
  int fourth_thu = first_thu + 21;
  return mday == fourth_thu;
}

// ----------------------------------------------------------------- public ---

Holiday holidayToday(int* out_month, int* out_day) {
  int mon, mday, wday;
  if (!localDate(mon, mday, wday)) return Holiday::None;
  if (out_month) *out_month = mon;
  if (out_day)   *out_day   = mday;

  // Birthday wins over all others when set
  if (g_set.birthdayMonth > 0 && g_set.birthdayDay > 0 &&
      mon == g_set.birthdayMonth && mday == g_set.birthdayDay) {
    return Holiday::Birthday;
  }

  if (!g_set.holidaysOn) return Holiday::None;

  if (mon == 1  && mday == 1)  return Holiday::NewYear;
  if (mon == 1  && mday == 29) return Holiday::PatentDay;
  if (mon == 12 && mday == 31) return Holiday::NYE;
  if (mon == 2  && mday == 14) return Holiday::Valentines;
  if (mon == 7  && mday == 4)  return Holiday::July4;
  if (mon == 10 && mday == 31) return Holiday::Halloween;
  if (isThanksgiving(mon, mday))   return Holiday::Thanksgiving;
  if (mon == 12 && mday == 24) return Holiday::ChristmasEve;
  if (mon == 12 && mday == 25) return Holiday::Christmas;

  return Holiday::None;
}

// Tracks the first-wake display for today
RTC_DATA_ATTR static uint16_t s_greeted_yday = 0xFFFF;  // 0=Jan1
// ...and which holiday that was, so setting the birthday to today (menu, BLE, radio)
// after the day's first greeting still gets the birthday screen + all-day hat.
RTC_DATA_ATTR static uint8_t  s_greeted_hol = (uint8_t)Holiday::None;

bool holidayDayGreeted() {
  int mon, mday, wday;
  if (!localDate(mon, mday, wday)) return true;
  struct tm t; time_t now = rtcNowUtc() + (time_t)tzOffsetMin() * 60;
  gmtime_r(&now, &t);
  uint16_t yday = (uint16_t)t.tm_yday;
  return s_greeted_yday == yday;
}

void holidayDayTick() { /* used externally to clear — handled via RTC attr */ }

// All-day costume: set for the rest of the day after first wake
RTC_DATA_ATTR static uint16_t s_costume_yday = 0xFFFF;
RTC_DATA_ATTR static uint8_t  s_costume_holiday = (uint8_t)Holiday::None;

bool holidayCostumeOn() {
  int mon, mday, wday;
  if (!localDate(mon, mday, wday)) return false;
  time_t now = rtcNowUtc() + (time_t)tzOffsetMin() * 60;
  struct tm t; gmtime_r(&now, &t);
  uint16_t yday = (uint16_t)t.tm_yday;
  return s_costume_yday == yday && s_costume_holiday != (uint8_t)Holiday::None;
}

Holiday holidayCostumeType() {
  if (!holidayCostumeOn()) return Holiday::None;
  return (Holiday)s_costume_holiday;
}

static void armCostume(Holiday h) {
  time_t now = rtcNowUtc() + (time_t)tzOffsetMin() * 60;
  struct tm t; gmtime_r(&now, &t);
  s_costume_yday    = (uint16_t)t.tm_yday;
  s_costume_holiday = (uint8_t)h;
}

bool holidayGreet() {
  int mon, mday, wday;
  if (!localDate(mon, mday, wday)) return false;

  time_t now = rtcNowUtc() + (time_t)tzOffsetMin() * 60;
  struct tm t; gmtime_r(&now, &t);
  uint16_t yday = (uint16_t)t.tm_yday;

  Holiday h = holidayToday();
  if (s_greeted_yday == yday && s_greeted_hol == (uint8_t)h) return false;  // already showed today
  s_greeted_yday = yday;
  s_greeted_hol = (uint8_t)h;
  if (h == Holiday::None) return false;

  // Skip non-birthday holidays if greetings off, but always show birthday
  if (!g_set.greetOn && h != Holiday::Birthday) return false;

  armCostume(h);

  switch (h) {

    case Holiday::Birthday:
      brainSetGreeting("Alles Gute!", "HAPPY BIRTHDAY!", Eye::Happy);
      brainEventPrio(Expr::HolidayBirthday, 4000, 95);
      audioPlayClip("geburtstag");
      break;

    case Holiday::NewYear:
      brainSetGreeting("Prost Neujahr!", "HAPPY NEW YEAR!", Eye::Happy);
      brainEventPrio(Expr::HolidayNewYear, 4000, 95);
      audioPlayClip("neujahr");
      break;

    case Holiday::NYE:
      brainSetGreeting("Guten Rutsch!", "HAPPY NEW YEAR'S EVE", Eye::Happy);
      brainEventPrio(Expr::Greeting, 2600, 95);
      audioPlayClip("rutsch");
      break;

    case Holiday::Valentines:
      brainSetGreeting("Alles Liebe!", "HAPPY VALENTINE'S", Eye::Happy);
      brainEventPrio(Expr::Greeting, 2600, 95);
      audioPlayClip("liebe");
      break;

    case Holiday::July4:
      brainSetGreeting("HAPPY 4TH!", "JULY 4TH", Eye::Happy);
      brainEventPrio(Expr::HolidayJuly4, 4000, 95);
      audioPlayClip("glueckwunsch");
      break;

    case Holiday::Halloween:
      brainSetGreeting("Süßes oder Saures!", "TRICK OR TREAT", Eye::Cross);
      brainEventPrio(Expr::HolidayHalloween, 4000, 95);
      audioPlayClip("halloween");
      break;

    case Holiday::Thanksgiving:
      brainSetGreeting("Erntedank!", "HAPPY THANKSGIVING", Eye::Happy);
      brainEventPrio(Expr::Greeting, 2600, 95);
      audioPlayClip("erntedank");
      break;

    case Holiday::ChristmasEve:
    case Holiday::Christmas:
      brainSetGreeting("Frohe Weihnachten!", "MERRY CHRISTMAS", Eye::Happy);
      brainEventPrio(Expr::HolidayChristmas, 4000, 95);
      audioPlayClip("weihnachten");
      break;

    case Holiday::PatentDay:
      brainEventPrio(Expr::HolidayPatent, 4000, 95);
      audioPlayClip("patent");
      break;

    default: return false;
  }
  return true;
}
