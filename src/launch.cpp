#include "launch.h"
#include "brain.h"
#include "audio.h"
#include "commands.h"
#include "motion.h"
#include "settings.h"
#include <Preferences.h>

// Drag race (docs/faces/21-drag-race.html):
//   armed -> IKUZO -> PRE-STAGE (waiting for the car to stop) -> STAGED -> tree -> GREEN
//   moving before green = RED LIGHT (run doesn't count); green -> first movement = reaction time.
//   With an outside speed source (`speed` command; none on Otto yet): live 0-60 run, then timeslip (R/T, 0-30, 0-60, peak g) and NEW RECORD.
//   Without one (Otto today): reaction-time-only timeslip (the motion chip still sees the launch).
// Not armed + LAUNCH on: any clean GPS run from a stop that reaches 60 in < 15 s still shows a slip.

enum class LState : uint8_t { Idle, Intro, Prestage, Staged, Tree, Green, Running };

static const float LAUNCH_G = 0.12f;          // sustained forward g that counts as "moved"
static const uint32_t LAUNCH_MS = 60;         // ...for this long
static const int PRIO = 80;                   // above braking/accel faces

static LState s_state = LState::Idle;
static bool s_armedRun = false;               // armed (tree) run: show live screen
static bool s_staged = false;                 // GPS says stopped: ready for an automatic run
static uint32_t s_armedAt = 0, s_stateAt = 0, s_treeStart = 0, s_stageFor = 0, s_green = 0;
static uint32_t s_t0 = 0, s_t30 = 0, s_tentative = 0, s_stopSince = 0, s_last = 0, s_moveMs = 0;
static uint32_t s_slipAt = 0;                 // show the timeslip at this time (after NEW RECORD)
static int s_step = 0;                        // tree bulbs already beeped
static float s_maxSpeed = 0, s_best = 0, s_bestRt = 0, s_rt = -1, s_peak = 0;

// signed age: a `speed` line read later in the same loop pass is a few ms *newer* than `now`, and the
// unsigned difference wrapped to ~4 billion = "stale" (the run flickered GPS / no GPS)
static bool speedValid(uint32_t now) { return g_speed.at && (int32_t)(now - g_speed.at) < 1500; }

void launchInit() {
  Preferences p;
  p.begin("otto", true);
  s_best = p.getFloat("best060", 0);
  s_bestRt = p.getFloat("bestrt", 0);
  p.end();
}

float launchBest() { return s_best; }
float launchBestRt() { return s_bestRt; }
bool launchRunning() { return s_state != LState::Idle; }

void launchResetBest() {
  s_best = 0; s_bestRt = 0;
  Preferences p;
  p.begin("otto", false);
  p.remove("best060");
  p.remove("bestrt");
  p.end();
}

// Serial log of each step, so a bench/car test can be checked afterwards
static const char* const kStateNames[] = {"idle", "intro", "prestage", "staged", "tree", "green", "running"};
static void go(LState st, uint32_t now) {
  if (st != s_state) Serial.printf("launch: %s (lon %.2f g)\n", kStateNames[(int)st], g_motion.lon);
  s_state = st; s_stateAt = now;
}

static void fmtRt(char* b, size_t n, float rt) {
  if (rt < 0) snprintf(b, n, "--");
  else if (rt < 1) snprintf(b, n, ".%03d", (int)(rt * 1000 + 0.5f));
  else snprintf(b, n, "%.3f", rt);
}

void launchArm() {
  uint32_t now = millis();
  s_armedAt = now;
  s_tentative = 0; s_moveMs = 0; s_rt = -1;
  go(LState::Intro, now);
  brainSetGreeting("Los geht's!", "LET'S GO", Eye::Chevron);
  brainEventPrio(Expr::Greeting, 1800, PRIO);
  audioPlayClip("losgehts");
}

static void redLight(uint32_t now) {
  brainSetTree(TREE_PRESTAGE | TREE_RED);
  brainSetCaption("RED LIGHT!");
  brainEventPrio(Expr::LaunchRed, 3500, PRIO);
  audioChirp(320, 200, 500);
  Serial.println("launch: RED LIGHT");
  go(LState::Idle, now);
}

static void showSlip(const char* caption) {
  brainSetCaption(caption);
  brainEventPrio(Expr::LaunchResult, 15000, PRIO);   // a tap closes it early (touch.cpp)
}

static void startRun(uint32_t now, bool armed, uint32_t t0) {
  go(LState::Running, now);
  s_armedRun = armed;
  s_t0 = t0;
  s_t30 = 0;
  s_peak = 0;
  s_maxSpeed = g_speed.mph;
  s_staged = false;
}

// Reaction time only (no GPS speed for a 0-60)
static void finishRt(uint32_t now) {
  bool best = s_rt >= 0 && (s_bestRt <= 0 || s_rt < s_bestRt);
  if (best) {
    s_bestRt = s_rt;
    Preferences p;
    p.begin("otto", false);
    p.putFloat("bestrt", s_rt);
    p.end();
  }
  char rt[10], pk[10], cap[28];
  fmtRt(rt, sizeof(rt), s_rt);
  snprintf(pk, sizeof(pk), "%.2fG", s_peak);
  brainSetSlip(rt, "", "", pk);
  if (best) snprintf(cap, sizeof(cap), "NEW BEST R/T!");
  else { char b[10]; fmtRt(b, sizeof(b), s_bestRt); snprintf(cap, sizeof(cap), "BEST R/T %s", b); }
  showSlip(cap);
  audioChirp(700, 1400, 120);
  Serial.printf("launch: R/T %s, peak %s (%s)\n", rt, pk, cap);
  go(LState::Idle, now);
}

static void finish(uint32_t now) {
  float secs = (now - s_t0) / 1000.0f;
  bool armed = s_armedRun;
  go(LState::Idle, now);
  s_tentative = 0;
  if (!armed && secs > 15.0f) return;           // ordinary acceleration, not a launch
  bool best = s_best <= 0 || secs < s_best;
  if (best) {
    s_best = secs;
    Preferences p;
    p.begin("otto", false);
    p.putFloat("best060", secs);
    p.end();
  }
  if (s_rt >= 0 && (s_bestRt <= 0 || s_rt < s_bestRt)) {
    s_bestRt = s_rt;
    Preferences p;
    p.begin("otto", false);
    p.putFloat("bestrt", s_rt);
    p.end();
  }
  char rt[10], t30[10], t60[10], pk[10], cap[28];
  fmtRt(rt, sizeof(rt), s_rt);
  if (s_t30) snprintf(t30, sizeof(t30), "%.1f", (s_t30 - s_t0) / 1000.0f); else snprintf(t30, sizeof(t30), "--");
  snprintf(t60, sizeof(t60), "%.1f", secs);
  snprintf(pk, sizeof(pk), "%.2fG", s_peak);
  brainSetSlip(rt, t30, t60, pk);
  snprintf(cap, sizeof(cap), "BEST %.1f S", s_best);
  brainSetCaption(cap);
  if (best) {                                   // NEW RECORD first, then the slip
    char c2[24];
    snprintf(c2, sizeof(c2), "0-60 %.1f S", secs);
    brainSetGreeting("Neuer Rekord!", "NEW RECORD", Eye::Happy);
    brainSetCaption(c2);
    brainEventPrio(Expr::LaunchRecord, 3500, PRIO);
    audioPlayClip("rekord");
    s_slipAt = now + 3500;
  } else {
    showSlip(cap);
    audioChirp(700, 1400, 120);
    audioChirp(1400, 2100, 160);
  }
}

void launchUpdate(uint32_t now) {
  uint32_t dt = s_last ? now - s_last : 0;
  s_last = now;
  if (dt > 200) dt = 200;
  bool sv = speedValid(now);
  float spd = sv ? g_speed.mph : 0;

  // the motion chip sees the car move before GPS does: sustained forward g
  bool fwd = g_motion.ok && g_motion.lon > LAUNCH_G;
  s_moveMs = fwd ? s_moveMs + dt : 0;
  bool moved = s_moveMs >= LAUNCH_MS;
  uint32_t movedAt = now - s_moveMs;            // when the push started
  bool stopped = g_motion.stoppedSecs > 1.0f && (!sv || spd < 0.5f);

  // GPS stop detection for automatic (not armed) runs
  if (sv && spd < 0.5f) { if (!s_stopSince) s_stopSince = now; }
  else s_stopSince = 0;
  if (s_stopSince && now - s_stopSince > 1000) s_staged = true;
  if (s_state == LState::Idle && s_staged && spd < 0.5f) {
    if (!s_tentative && g_motion.lon > LAUNCH_G) s_tentative = now;
    if (s_tentative && now - s_tentative > 1500) s_tentative = 0;   // false alarm
  }

  // timeslip after the NEW RECORD screen
  if (s_slipAt && (int32_t)(now - s_slipAt) >= 0) {
    s_slipAt = 0;
    char cap[28];
    snprintf(cap, sizeof(cap), "BEST %.1f S", s_best);
    showSlip(cap);
  }

  bool armedState = s_state != LState::Idle && s_state != LState::Running;
  if (armedState && now - s_armedAt > 120000) { go(LState::Idle, now); return; }   // gave up

  switch (s_state) {
    case LState::Idle:
      if (g_set.launchOn && s_staged && sv && spd >= 0.5f) {
        s_rt = -1;
        startRun(now, false, s_tentative ? s_tentative : now);
      }
      break;

    case LState::Intro:                         // "Los geht's!" greeting is showing
      if (now - s_stateAt > 1800) go(LState::Prestage, now);
      break;

    case LState::Prestage:
      brainSetTree(TREE_PRESTAGE);
      brainSetCaption("PRE-STAGE");
      brainEventPrio(Expr::LaunchReady, 300, PRIO);
      if (stopped && !fwd) {
        go(LState::Staged, now);
        s_stageFor = 1500 + esp_random() % 800;  // like an auto-start: a short random wait
        audioChirp(900, 900, 60);
      }
      break;

    case LState::Staged:
      brainSetTree(TREE_PRESTAGE | TREE_STAGE);
      brainSetCaption("STAGED");
      brainEventPrio(Expr::LaunchReady, 300, PRIO);
      if (moved) { redLight(now); break; }      // rolled out of the beams
      if (now - s_stateAt >= s_stageFor) { go(LState::Tree, now); s_treeStart = now; s_step = 0; }
      break;

    case LState::Tree: {
      uint32_t el = now - s_treeStart;
      bool pro = g_set.treePro;
      uint32_t greenAt = pro ? 400 : 1500;
      uint8_t b = TREE_PRESTAGE | TREE_STAGE;
      int lit = pro ? 3 : min(3, (int)(el / 500) + 1);
      if (lit >= 1) b |= TREE_A1;
      if (lit >= 2) b |= TREE_A2;
      if (lit >= 3) b |= TREE_A3;
      if (lit > s_step) { s_step = lit; audioChirp(1000, 1000, pro ? 200 : 90); }
      brainSetTree(b);
      brainSetCaption(pro ? "TREE .400 PRO" : "TREE .500");
      brainEventPrio(Expr::LaunchTree, 300, PRIO);
      if (moved) { redLight(now); break; }      // jumped the start
      if (el >= greenAt) {
        s_green = s_treeStart + greenAt;
        go(LState::Green, now);
        audioChirp(1600, 1600, 260);
      }
      break;
    }

    case LState::Green:
      brainSetTree(TREE_PRESTAGE | TREE_STAGE | TREE_GREEN);
      brainEventPrio(Expr::LaunchGo, 300, PRIO);
      if (moved) {
        if ((int32_t)(movedAt - s_green) < 0) { redLight(now); break; }   // started moving before green
        s_rt = (movedAt - s_green) / 1000.0f;
        startRun(now, true, movedAt);
        break;
      }
      if (now - s_green > 5000) {               // never launched
        brainSetCaption("NO LAUNCH");
        brainEventPrio(Expr::LaunchReady, 1500, PRIO);
        go(LState::Idle, now);
      }
      break;

    case LState::Running: {
      float secs = (now - s_t0) / 1000.0f;
      s_peak = fmaxf(s_peak, g_motion.lon);
      if (!sv) {                                // no GPS: reaction time + peak g only
        if (s_armedRun) {                       // keep GO! up (else the 5.0 / accel face takes over)
          brainSetTree(TREE_PRESTAGE | TREE_STAGE | TREE_GREEN);
          brainEventPrio(Expr::LaunchGo, 300, PRIO);
        }
        if (s_armedRun && secs > 1.5f) finishRt(now);
        else if (!s_armedRun) go(LState::Idle, now);
        break;
      }
      if (secs > 20.0f || spd < s_maxSpeed - 3.0f) {   // gave up / lifted
        if (s_armedRun) finishRt(now); else go(LState::Idle, now);
        s_tentative = 0;
        break;
      }
      s_maxSpeed = fmaxf(s_maxSpeed, spd);
      if (!s_t30 && spd >= 30.0f) s_t30 = now;
      if (s_armedRun) {
        char big[8], sub[28], lab[16];
        snprintf(big, sizeof(big), "%d", (int)(spd + 0.5f));
        snprintf(sub, sizeof(sub), "MPH - %.1f S", secs);
        char rt[10];
        fmtRt(rt, sizeof(rt), s_rt);
        snprintf(lab, sizeof(lab), "R/T %s", rt);
        brainSetAnswer(lab, big, sub);
        brainSetRing(spd / 60.0f);
        brainEventPrio(Expr::LaunchRun, 300, PRIO);
      }
      if (spd >= 60.0f) finish(now);
      break;
    }
  }
}
