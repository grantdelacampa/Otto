#include "extras.h"
#include "brain.h"
#include "audio.h"
#include "board.h"
#include "motion.h"
#include "settings.h"
#include "holidays.h"
#include "trip.h"
#include "touch.h"
#include <Preferences.h>

// ------------------------------------------------------------- helpers --
static void fmtClock(time_t utc, char* b, size_t n) {    // "3:14 PM" in local time
  time_t loc = utc + (time_t)tzOffsetMin() * 60;
  struct tm t;
  gmtime_r(&loc, &t);
  int h = t.tm_hour % 12;
  snprintf(b, n, "%d:%02d %s", h ? h : 12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
}

static uint32_t prefGet(const char* key) {
  Preferences p;
  p.begin("otto", true);
  uint32_t v = p.getUInt(key, 0);
  p.end();
  return v;
}

static void prefPut(const char* key, uint32_t v) {
  Preferences p;
  p.begin("otto", false);
  p.putUInt(key, v);
  p.end();
}

// ---------------------------------------------------------------- guard --
// Bumps logged while asleep (survive deep sleep, not a power loss)
static const int GUARD_MAX = 8;
RTC_DATA_ATTR static uint8_t s_gN = 0;
RTC_DATA_ATTR static uint32_t s_gT[GUARD_MAX];
RTC_DATA_ATTR static uint32_t s_gTotal = 0;

void extrasGuardBump() {
  if (!g_set.guardOn) return;
  uint32_t t = rtcValid() ? (uint32_t)rtcNowUtc() : 0;
  if (s_gN < GUARD_MAX) s_gT[s_gN++] = t;
  else { memmove(s_gT, s_gT + 1, sizeof(uint32_t) * (GUARD_MAX - 1)); s_gT[GUARD_MAX - 1] = t; }
  s_gTotal++;
}

// ------------------------------------------------------- wake + cards --
enum Card : uint8_t { C_NONE, C_GUARD, C_HOT, C_COLD };
static Card s_cards[3];
static int s_nCards = 0;
static char s_guardTime[12] = "", s_guardSub[16] = "";
static int s_cabinC = 0;
static bool s_missed = false;
static uint32_t s_daysParked = 0;
RTC_DATA_ATTR static uint32_t s_lastCabinCard = 0;  // utc: at most one cabin card an hour

void extrasOnWake(bool coldBoot) {
  s_nCards = 0;
  uint32_t now = rtcValid() ? (uint32_t)rtcNowUtc() : 0;

  // Guard: drop the bumps from the last 2 minutes (that was you getting in), report the rest
  int keep = 0;
  uint32_t lastT = 0;
  for (int i = 0; i < s_gN; i++) {
    bool recent = now && s_gT[i] && now - s_gT[i] < 120;
    if (!recent) { keep++; lastT = s_gT[i]; }
  }
  if (keep && g_set.guardOn) {
    if (lastT) fmtClock(lastT, s_guardTime, sizeof(s_guardTime));
    else snprintf(s_guardTime, sizeof(s_guardTime), "WHILE PARKED");
    snprintf(s_guardSub, sizeof(s_guardSub), keep == 1 ? "1 BUMP" : (keep >= GUARD_MAX ? "%d+ BUMPS" : "%d BUMPS"), keep);
    s_cards[s_nCards++] = C_GUARD;
    Serial.printf("guard: %d bump(s) while parked, last %s\n", keep, s_guardTime);
  }
  s_gN = 0;

  // Cabin: the motion chip's temperature right after waking, before Otto warms itself up
  // The chip was just reset in motionInit(): its temperature register reads 0 until the first
  // measurement, which showed the COLD card on every boot. Wait (up to ~200 ms) for a real one.
  float c = 0;
  bool got = false;
  for (int i = 0; i < 20 && !got; i++) {
    got = motionReadTemp(c) && c != 0.0f && c > -40 && c < 100;
    if (!got) delay(10);
  }
  if (got) Serial.printf("cabin: %.1f C on wake\n", c);
  else Serial.println("cabin: no temperature reading");
  // Only after 10+ min asleep/off: right after a reset or a short nap the chip still reads the board's
  // own warmth (bench, Nov 2026: 39.7 C after a flash on USB -> HOT card on every boot).
  uint32_t offAt = prefGet("offAt");
  prefPut("offAt", 0);
  bool cooled = now && offAt && now > offAt && now - offAt >= 600;
  if (g_set.cabinOn && got && cooled && now - s_lastCabinCard > 3600) {
    s_cabinC = (int)lroundf(c);
    if (s_cabinC >= g_set.hotC) { s_cards[s_nCards++] = C_HOT; s_lastCabinCard = now; }
    else if (s_cabinC <= g_set.coldC) { s_cards[s_nCards++] = C_COLD; s_lastCabinCard = now; }
  }

  // Missed you: last awake 7+ days ago (needs the clock)
  uint32_t last = prefGet("lastSeen");
  s_missed = false;
  if (now && last && now > last) {
    s_daysParked = (now - last) / 86400;
    s_missed = g_set.milestoneOn && g_set.greetOn && s_daysParked >= 7;
  }
  if (now) prefPut("lastSeen", now);
  (void)coldBoot;
}

bool extrasGreet() {
  if (!s_missed) return false;
  s_missed = false;
  if (holidayToday() != Holiday::None) return false;     // birthdays and holidays win
  char cap[24];
  snprintf(cap, sizeof(cap), "%u DAYS PARKED", (unsigned)s_daysParked);
  brainSetGreeting("Lange nicht gesehen!", "LONG TIME NO SEE", Eye::Happy);
  brainSetCaption(cap);
  brainEvent(Expr::MissedYou, 4000);
  audioPlayClip("langenicht");
  return true;
}

void extrasOnSleep() {
  if (rtcValid()) { prefPut("lastSeen", (uint32_t)rtcNowUtc()); prefPut("offAt", (uint32_t)rtcNowUtc()); }
}

bool extrasCardPending() { return s_nCards > 0; }

static void showCard(Card c) {
  char b[24];
  switch (c) {
    case C_GUARD:
      brainSetAnswer("SOMEONE BUMPED ME", s_guardTime, s_guardSub);
      brainEvent(Expr::Guard, 5000);
      audioChirp(500, 300, 180);
      break;
    case C_HOT:
      snprintf(b, sizeof(b), "HOT - %dC", s_cabinC);
      brainSetGreeting("Heiß!", b, Eye::Half);
      brainSetCaption("AIR IT OUT");
      brainEvent(Expr::HotCabin, 4000);
      audioPlayClip("heiss");
      break;
    case C_COLD:
      snprintf(b, sizeof(b), "COLD - %dC", s_cabinC);
      brainSetGreeting("Kalt!", b, Eye::Dot);
      brainSetCaption("BRRR!");
      brainEvent(Expr::ColdCabin, 4000);
      audioPlayClip("kalt");
      break;
    default: break;
  }
}

// ----------------------------------------------------------- milestones --
static bool s_milestone = false;
static uint32_t s_drives = 0;

static bool isMilestone(uint32_t n) {
  static const uint32_t kM[] = {10, 25, 50, 100, 150, 200, 250, 300, 400, 500, 750, 1000};
  for (uint32_t m : kM) if (n == m) return true;
  return n > 1000 && n % 500 == 0;
}

void extrasDriveDone() {
  s_drives = prefGet("drives") + 1;
  prefPut("drives", s_drives);
  if (g_set.milestoneOn && isMilestone(s_drives)) s_milestone = true;
}

uint32_t extrasDriveCount() { return s_drives ? s_drives : prefGet("drives"); }

bool extrasShowMilestone() {
  if (!s_milestone) return false;
  s_milestone = false;
  char big[12];
  snprintf(big, sizeof(big), "%u", (unsigned)s_drives);
  brainSetAnswer("WE MADE IT", big, "DRIVES TOGETHER");
  brainEvent(Expr::Milestone, 5000);
  audioPlayClip("glueckwunsch");
  return true;
}

// ------------------------------------------------------------ car show --
static bool s_show = false;
static uint32_t s_showStart = 0, s_showNext = 0, s_showMoving = 0;
static int s_showStep = 0;
static uint32_t s_cardCheck = 0;

void extrasShow(bool on) {
  if (on && !s_show) { s_showStart = millis(); s_showNext = 0; s_showStep = 0; s_showMoving = 0; s_cardCheck = 0; }
  if (!on && s_show) brainEvent(Expr::Happy, 1000);
  s_show = on;
  Serial.printf("show mode %s\n", on ? "on" : "off");
}

bool extrasShowOn() { return s_show; }

static void showUpdate(uint32_t now) {
  if (now - s_showStart > (uint32_t)g_set.showMin * 60000UL) { extrasShow(false); return; }
  // (same rule as the menu: a sustained push or turn, so taps and handling don't count)
  bool moving = g_motion.ok && (fabsf(g_motion.lon) > 0.15f || fabsf(g_motion.lat) > 0.15f || fabsf(g_motion.yaw) > 15.0f);
  static uint32_t last = now;
  uint32_t dt = now - last; last = now;
  if (dt > 200) dt = 200;
  // a touch moves the board too (and a hand-held board reads as accelerating): ignore movement
  // from just before until 5 s after a finger was down
  static uint32_t s_touchAt = 0;
  if (touchActive()) s_touchAt = now;
  if (s_touchAt && now - s_touchAt < 5000) moving = false;
  s_showMoving = moving ? s_showMoving + dt : (s_showMoving > 2 * dt ? s_showMoving - 2 * dt : 0);
  if (s_showMoving > 3000) { Serial.println("show: drove off"); extrasShow(false); return; }
  // a moment after putting the card up: if something higher still had the screen (menu closing,
  // a tap reaction) the card was dropped -> put it up again shortly
  if (s_cardCheck && (int32_t)(now - s_cardCheck) >= 0) {
    s_cardCheck = 0;
    if (brainCurrent() != Expr::ShowCard) { s_showStep--; s_showNext = now + 250; }
  }
  if ((int32_t)(now - s_showNext) < 0) return;
  // attract loop: the spec card, then a little trick, repeat
  if (s_showStep % 2 == 0) {
    brainSetAnswer(g_set.showL1, g_set.showL2, "TAP ME!");
    brainEvent(Expr::ShowCard, 6000);
    s_showNext = now + 6000;
    s_cardCheck = now + 300;
  } else {
    static const Expr kTricks[] = {Expr::TapHappy, Expr::Wobble, Expr::Spinning, Expr::Pet, Expr::V8, Expr::Boop};
    Expr e = kTricks[esp_random() % 6];
    if (e == Expr::Spinning) brainSpinImpulse((esp_random() & 1 ? 1 : -1) * 1400.0f);
    brainEvent(e, 2500);
    s_showNext = now + 2600;
  }
  s_showStep++;
}

// ----------------------------------------------------------- break time --
static uint32_t s_breakTrip = 0;     // trip start (ms) we already reminded for

// ------------------------------------------------------------------ loop --
void extrasUpdate(uint32_t now, bool ready) {
  if (s_show) showUpdate(now);
  if (!ready) return;

  static uint32_t s_nextCard = 0;
  if (s_nCards && (int32_t)(now - s_nextCard) >= 0) {
    showCard(s_cards[0]);
    for (int i = 1; i < s_nCards; i++) s_cards[i - 1] = s_cards[i];
    s_nCards--;
    s_nextCard = now + 5200;
    return;
  }
  if (s_milestone && (int32_t)(now - s_nextCard) >= 0) { extrasShowMilestone(); s_nextCard = now + 5200; return; }

  // Pause? after ~2 h on the road, at the next stop, once per drive
  uint32_t start = tripStartMs();
  if (g_set.breakOn && start && start != s_breakTrip && now - start >= 2UL * 3600 * 1000 &&
      g_motion.stoppedSecs > 5.0f) {
    s_breakTrip = start;
    uint32_t mins = (now - start) / 60000;
    char b[20];
    snprintf(b, sizeof(b), "%u H %u MIN", (unsigned)(mins / 60), (unsigned)(mins % 60));
    brainSetGreeting("Pause?", "TIME FOR A BREAK?", Eye::Content);
    brainSetCaption(b);
    brainEvent(Expr::BreakTime, 6000);
    audioPlayClip("pause");
  }
}

// ---------------------------------------------------------------- bench --
void extrasDebug(const char* w, char* r, size_t n) {
  if (!strcasecmp(w, "guard")) {                 // pretend two bumps happened an hour ago
    uint32_t now = rtcValid() ? (uint32_t)rtcNowUtc() : 0;
    s_gN = 0; s_gT[s_gN++] = now ? now - 4000 : 0; s_gT[s_gN++] = now ? now - 3600 : 0;
    extrasOnWake(false);
    snprintf(r, n, "guard card queued");
  } else if (!strcasecmp(w, "hot") || !strcasecmp(w, "cold")) {
    s_cabinC = w[0] == 'h' ? 54 : 2;
    s_cards[0] = w[0] == 'h' ? C_HOT : C_COLD; s_nCards = 1;
    snprintf(r, n, "cabin card queued");
  } else if (!strcasecmp(w, "missed")) {
    s_daysParked = 8; s_missed = true;
    if (!extrasGreet()) snprintf(r, n, "not shown (holiday today?)"); else snprintf(r, n, "ok");
  } else if (!strcasecmp(w, "milestone")) {
    s_drives = 100; s_milestone = true;
    snprintf(r, n, "milestone queued");
  } else if (!strcasecmp(w, "break")) {
    brainSetGreeting("Pause?", "TIME FOR A BREAK?", Eye::Content);
    brainSetCaption("2 H 10 MIN");
    brainEvent(Expr::BreakTime, 6000);
    audioPlayClip("pause");
    snprintf(r, n, "ok");
  } else if (!strcasecmp(w, "patent")) {
    brainEventPrio(Expr::HolidayPatent, 4000, 95);
    audioPlayClip("patent");
    snprintf(r, n, "ok");
  } else {
    snprintf(r, n, "extras: drives %u, guard log %d (total %u), show %s - try: extras guard|hot|cold|missed|milestone|break|patent",
             (unsigned)extrasDriveCount(), s_gN, (unsigned)s_gTotal, s_show ? "on" : "off");
  }
}
