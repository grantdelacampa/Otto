#include "greetings.h"
#include "brain.h"
#include "board.h"
#include "audio.h"
#include "settings.h"
#include "holidays.h"

enum When : uint8_t { ANY, MORNING, DAY, EVENING, LATE };

// Otto greets in German (an unflappable 1997 E420), with the English meaning underneath.
struct Greeting { const char* greet; const char* gloss; const char* clip; When when; Eye eye; };

static const Greeting kGreetings[] = {
  {"Guten Morgen!",  "GOOD MORNING",  "morgen",     MORNING, Eye::Happy},
  {"Guten Tag!",     "GOOD DAY",      "tag",        DAY,     Eye::Happy},
  {"Guten Abend!",   "GOOD EVENING",  "abend",      EVENING, Eye::Happy},
  {"Willkommen!",    "WELCOME BACK",  "willkommen", ANY,     Eye::Dot},
  {"Hallo!",         "HELLO",         "hallo",      ANY,     Eye::Happy},
  {"Los geht's!",    "LET'S GO",      "losgehts",   ANY,     Eye::Chevron},
  {"Bereit.",        "READY",         "bereit",     ANY,     Eye::Dot},
  {"Müde…",          "SLEEPY",        "muede",      LATE,    Eye::Closed},
};
static const int N = sizeof(kGreetings) / sizeof(kGreetings[0]);

RTC_DATA_ATTR static int8_t s_last = -1;  // survives deep sleep

static When nowBucket() {
  int h = localHour();
  if (h < 0) return ANY;                 // clock unknown: only time-neutral greetings
  if (h >= 5 && h < 11) return MORNING;
  if (h >= 11 && h < 18) return DAY;
  if (h >= 18 && h < 23) return EVENING;
  return LATE;
}

void greetNow() {
  // Holiday first-wake screen replaces the normal greeting on special days.
  // holidayGreet() returns true if it took over (and arms the all-day costume).
  if (holidayGreet()) return;

  if (!g_set.greetOn) return;
  When b = nowBucket();
  int cand[N], n = 0;
  for (int i = 0; i < N; i++) {
    if (i == s_last) continue;
    When w = kGreetings[i].when;
    if (w == ANY || (b != ANY && w == b)) {
      cand[n++] = i;
      if (b != ANY && w == b) cand[n++] = i;  // time-of-day greetings twice as likely
    }
  }
  if (!n) cand[n++] = 3;
  int pick = cand[esp_random() % n];
  s_last = pick;
  const Greeting& g = kGreetings[pick];
  brainSetGreeting(g.greet, g.gloss, g.eye);
  brainEvent(Expr::Greeting, 2600);
  audioPlayClip(g.clip);
}
