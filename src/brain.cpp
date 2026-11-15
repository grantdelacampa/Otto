#include "brain.h"
#include "settings.h"
#include "config.h"
#include "holidays.h"
#include "menu.h"

static const char* kNames[] = {
  "idle", "bored", "musicbob",
  "listening", "thinking", "speaking",
  "happy", "sleepy", "volume", "confused", "answer",
  "braking", "turnleft", "turnright", "accel", "bump", "dizzy", "v8", "calibrate",
  "boot", "charging", "lowbattery", "batterylevel", "chargesoon", "charged", "overheat", "sleep",
  "greeting", "boop",
  "taphappy", "wobble", "startled", "pet", "grumpy", "spinning", "menuopening", "menulocked", "saved", "sehrgut",
  "engineon", "rev", "launchready", "launchrun", "launchresult", "launchtree", "launchgo", "launchred", "launchrecord", "summary", "updating", "hotoff",
  "holidaybirthday", "holidaynewyear", "holidayjuly4", "holidayhalloween", "holidaychristmas",
  "guard", "missedyou", "milestone", "breaktime", "hotcabin", "coldcabin", "showcard", "holidaypatent",
  "warmup", "warm", "enginehot", "checkengine",
  "wifitune",
};
static_assert(sizeof(kNames) / sizeof(kNames[0]) == (size_t)Expr::COUNT, "names out of sync");

const char* exprName(Expr e) { return kNames[(int)e]; }
bool exprFromName(const char* n, Expr& out) {
  for (int i = 0; i < (int)Expr::COUNT; i++)
    if (!strcasecmp(n, kNames[i])) { out = (Expr)i; return true; }
  return false;
}

static int defaultPrio(Expr e) {
  switch (e) {
    case Expr::Boot: return 100;
    case Expr::Calibrate: return 90;
    case Expr::Overheat: return 80;
    case Expr::V8: return 65;
    case Expr::Braking: case Expr::Bump: return 60;
    case Expr::Dizzy: return 56;   // above turns: it fires during the 5th turn, which kept re-showing TurnLeft/Right
    case Expr::Accel: case Expr::TurnLeft: case Expr::TurnRight: return 55;
    case Expr::Volume: case Expr::Confused: case Expr::Answer: return 45;
    case Expr::Updating: return 110;
    case Expr::HotOff: return 105;
    case Expr::MenuOpening: return 85;
    case Expr::LaunchRun: return 75;
    case Expr::LaunchResult: return 66;
    case Expr::LaunchTree: case Expr::LaunchGo: case Expr::LaunchRed: case Expr::LaunchRecord: return 80;
    case Expr::MenuLocked: return 60;
    case Expr::LaunchReady: return 48;
    case Expr::Summary: return 46;
    case Expr::Rev: return 45;
    case Expr::SehrGut: case Expr::Warm: return 42;
    case Expr::WarmUp: return 43;
    case Expr::CheckEngine: case Expr::WifiTune: return 47;
    case Expr::EngineHot: return 62;
    case Expr::Guard: case Expr::Milestone: case Expr::BreakTime: case Expr::HotCabin: case Expr::ColdCabin: return 44;
    case Expr::MissedYou: return 41;
    case Expr::HolidayPatent: return 95;
    case Expr::ShowCard: return 26;
    case Expr::EngineOn: case Expr::Saved: return 40;
    case Expr::TapHappy: case Expr::Wobble: case Expr::Startled: case Expr::Pet: case Expr::Grumpy:
    case Expr::Spinning: return 30;
    case Expr::Greeting: return 40;
    case Expr::LowBattery: return 35;
    case Expr::Charging: return 30;
    case Expr::ChargeSoon: return 34;
    case Expr::BatteryLevel: case Expr::Charged: return 32;
    case Expr::Boop: case Expr::Happy: return 25;
    default: return 20;
  }
}

static const int VOICE_PRIO = 70;

struct Active { Expr e = Expr::Idle; uint32_t until = 0, start = 0; int prio = 0; bool on = false; };
static Active s_ev;
static Expr s_voice = Expr::Idle;
static uint32_t s_voiceUntil = 0;
static Expr s_cur = Expr::Idle;
static uint32_t s_curStart = 0;
static uint32_t s_now = 0;

static FaceText s_txt;
static Eye s_greetEye = Eye::Happy;
static int s_volume = 50;

// idle layers
enum class Micro : uint8_t { None, Glance, LookUp, Wobble, Yawn };
static Micro s_micro = Micro::None;
static uint32_t s_microStart = 0, s_microDur = 0, s_nextMicro = 0;
static uint32_t s_nextBlink = 0, s_blinkStart = 0;
static bool s_double = false;

FaceText& brainText() { return s_txt; }
Expr brainCurrent() { return s_cur; }

void brainInit() {
  s_now = millis();
  s_nextBlink = s_now + 2000;
  s_nextMicro = s_now + 15000;
}

void brainEventPrio(Expr e, uint32_t dur, int prio) {
  uint32_t now = millis();
  bool expired = !s_ev.on || (int32_t)(now - s_ev.until) >= 0;
  if (s_ev.on && !expired && s_ev.e == e) {             // same thing again: extend
    if ((int32_t)(now + dur - s_ev.until) > 0) s_ev.until = now + dur;
    return;
  }
  if (expired || prio >= s_ev.prio) {
    s_ev.e = e; s_ev.prio = prio; s_ev.start = now; s_ev.until = now + dur; s_ev.on = true;
  }
}

void brainEvent(Expr e, uint32_t dur) { brainEventPrio(e, dur, defaultPrio(e)); }

void brainVoice(Expr e) {
  if (e == Expr::Idle) { s_voice = Expr::Idle; return; }
  s_voice = e;
  s_voiceUntil = millis() + 15000;  // safety timeout
}

// Copy with truncation that never splits a UTF-8 character (umlauts are 2 bytes)
static void copyStr(char* dst, size_t n, const char* src) {
  strncpy(dst, src ? src : "", n - 1);
  dst[n - 1] = 0;
  size_t len = strlen(dst);
  if (src && strlen(src) > len) {                       // truncated: drop a partial character
    size_t i = len;
    while (i > 0 && ((uint8_t)dst[i - 1] & 0xC0) == 0x80) i--;   // back over continuation bytes
    if (i > 0 && ((uint8_t)dst[i - 1] & 0x80)) {               // lead byte: is its sequence complete?
      uint8_t lead = (uint8_t)dst[i - 1];
      size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
      if (len - (i - 1) < need) dst[i - 1] = 0;
    }
  }
}

void brainSetGreeting(const char* greet, const char* gloss, Eye eye) {
  copyStr(s_txt.greet, sizeof(s_txt.greet), greet);
  copyStr(s_txt.gloss, sizeof(s_txt.gloss), gloss);
  s_greetEye = eye;
}
void brainSetAnswer(const char* l, const char* b, const char* s) {
  copyStr(s_txt.label, sizeof(s_txt.label), l);
  copyStr(s_txt.big, sizeof(s_txt.big), b);
  copyStr(s_txt.sub, sizeof(s_txt.sub), s);
}

// ------------------------------------------------------ user crank + ring --
static float s_userSpin = 0, s_spinPeak = 0;
static float s_ring = 0;

void brainSpinImpulse(float dps) {
  s_userSpin += dps;
  s_userSpin = constrain(s_userSpin, -3000.0f, 3000.0f);
  s_spinPeak = fmaxf(s_spinPeak, fabsf(s_userSpin));
}

float brainUserSpin() { return s_userSpin; }
void brainSpinDrag(float dps) {
  s_userSpin = constrain(dps, -3000.0f, 3000.0f);
  s_spinPeak = 0;
}
void brainSpinRelease(float dps, bool dizzy) {
  s_userSpin = constrain(dps, -3000.0f, 3000.0f);
  s_spinPeak = dizzy ? 1800 : fabsf(s_userSpin) * 0.5f;   // (peak < 1800: no dizzy)
}

float brainUserSpinStep(float dt) {
  s_userSpin *= expf(-dt / 0.9f);  // coasts to a stop in ~2-3 s
  if (fabsf(s_userSpin) < 120 && s_spinPeak > 0) {
    if (s_spinPeak >= 1800) brainEvent(Expr::Dizzy, 2200);  // a hard flick ends dizzy
    s_spinPeak = 0;
  }
  if (fabsf(s_userSpin) < 2) s_userSpin = 0;
  return s_userSpin;
}

void brainSetRing(float p) { s_ring = constrain(p, 0.0f, 1.0f); }
void brainSetVolume(int v) { s_volume = constrain(v, 0, 100); }
void brainSetTree(uint8_t bulbs) { s_txt.tree = bulbs; }
void brainSetSlip(const char* rt, const char* t30, const char* t60, const char* peak) {
  copyStr(s_txt.slip[0], sizeof(s_txt.slip[0]), rt);
  copyStr(s_txt.slip[1], sizeof(s_txt.slip[1]), t30);
  copyStr(s_txt.slip[2], sizeof(s_txt.slip[2]), t60);
  copyStr(s_txt.slip[3], sizeof(s_txt.slip[3]), peak);
}
void brainSetCaption(const char* t) { copyStr(s_txt.caption, sizeof(s_txt.caption), t); }

// ----------------------------------------------------------- idle layers --
static uint32_t rnd(uint32_t lo, uint32_t hi) { return lo + esp_random() % (hi - lo + 1); }

static float updateBlink(uint32_t now) {
  if ((int32_t)(now - s_nextBlink) >= 0) {
    s_blinkStart = now;
    s_double = (esp_random() % 5) == 0;               // 20% double blink
    s_nextBlink = now + rnd(3000, 7000);
  }
  uint32_t dt = now - s_blinkStart;
  uint32_t len = s_double ? 420 : 160;
  if (dt > len) return 1.0f;
  float p = (float)(dt % 160) / 160.0f;                // one blink = 160 ms
  if (s_double && dt > 160 && dt < 260) return 1.0f;   // small gap
  return fmaxf(0.08f, 1.0f - sinf(p * PI));
}

static void updateMicro(uint32_t now, const BrainCtx& ctx) {
  if (s_micro != Micro::None && now - s_microStart > s_microDur) s_micro = Micro::None;
  if (s_micro == Micro::None && (int32_t)(now - s_nextMicro) >= 0) {
    int r = esp_random() % 3;
    if (ctx.night && g_set.yawnOn && (esp_random() % 3) == 0) r = 3;
    s_micro = (Micro)(r + 1);
    s_microStart = now;
    s_microDur = s_micro == Micro::Yawn ? 3500 : (s_micro == Micro::Wobble ? 2000 : 2600);
    // activity 1 -> ~70 s between behaviors, 10 -> ~12 s
    float base = 70000.0f - (g_set.activity - 1) * (58000.0f / 9.0f);
    s_nextMicro = now + (uint32_t)(base * (0.6f + 0.8f * (esp_random() % 1000) / 1000.0f));
  }
}

static void applyMicro(uint32_t now, FaceState& f) {
  if (s_micro == Micro::None) return;
  float p = (float)(now - s_microStart) / s_microDur;
  if (p > 1) p = 1;
  float env = sinf(p * PI);
  switch (s_micro) {
    case Micro::Glance: f.eyeDX = (p < 0.5f ? -12 : 12) * fminf(1.0f, env * 2.5f); break;
    case Micro::LookUp: f.eyeDY = -10 * env; f.eyeR += 1 * env; break;
    case Micro::Wobble: f.tilt = 9 * sinf(p * 4 * PI) * (1 - p); break;
    case Micro::Yawn: f.eye = Eye::Content; f.eyeDY = -6; f.ov |= OV_YAWN; break;
    default: break;
  }
}

// ------------------------------------------------------- expression map --
// Otto (docs/design/Otto.dc.html). f.spin is the crank speed in deg/s: 140 is the design's slow idle
// pump (one stroke per ~2.6 s), ~1200 its launch pump (0.3 s). rest = 1 settles the piston at the
// bottom of its stroke. Brows and mouth are Otto's; eye offsets are scaled down by the renderer.
static void build(Expr e, uint32_t now, uint32_t start, const BrainCtx& ctx, FaceState& f) {
  f = FaceState();
  f.captionColor = COL_PINK;
  float t = now / 1000.0f;
  char b[32];
  switch (e) {
    case Expr::Idle:                          // design IDLE 0x00: calm face, slow pump
      f.rbright = 0.85f + 0.15f * cosf(t * 2 * PI / 4.0f);  // breathe
      break;
    case Expr::Bored:
      f.eye = Eye::Half; f.eyeDY = 2; f.eyeDX = 6 * sinf(t * 2 * PI / 6.0f); f.mouth = Mouth::Small;
      f.ov |= OV_DOTS3; f.spin = 50;
      break;
    case Expr::MusicBob:                      // one stroke per beat
      f.eye = Eye::Happy; f.brow = Brow::Raised; f.mouth = Mouth::Smile; f.ov |= OV_NOTE;
      f.spin = ctx.bpm * 6;
      break;
    case Expr::Listening: f.eyeR = 10; f.brow = Brow::Raised; f.mouth = Mouth::Small; f.ov |= OV_SOUND; break;
    case Expr::Thinking:
      f.spin = 300; f.eyeR = 7; f.eyeDX = 6; f.eyeDY = -10; f.brow = Brow::Uneven; f.mouth = Mouth::Small;
      f.ov |= OV_THINKDOTS;
      break;
    case Expr::Speaking:  f.eyeDY = -4; f.brow = Brow::Raised; f.mouth = Mouth::Talk; break;
    case Expr::Happy: case Expr::Boop:
      f.eye = Eye::Happy; f.brow = Brow::Raised; f.mouth = Mouth::Smile; f.spin = 300;
      break;
    case Expr::Sleepy:                        // design SLEEPY 0x04: gray, eyes shut, resting at the bottom
      f.eye = Eye::Closed; f.brow = Brow::None; f.mouth = Mouth::Small; f.gray = true; f.rest = 1;
      f.eyeDY = 0; f.ov |= OV_ZZ;
      break;
    case Expr::Volume:    f.ov = OV_VOLUME; f.rbright = 0.6f; f.vol = s_volume; f.spin = 0; break;
    case Expr::Confused:  f.eye = Eye::Uneven; f.brow = Brow::Uneven; f.mouth = Mouth::Frown; f.tilt = -8; f.ov |= OV_QUESTION; break;
    case Expr::Answer:
      f.ov = OV_ANSWER; f.fcx = 120; f.fcy = 70; f.fscale = 0.42f; f.eyeR = 10;
      break;
    case Expr::Braking:                       // design DISAPPROVES 0x01: one brow up, "AHEM."
      f.eye = Eye::Ahem; f.brow = Brow::Ahem; f.mouth = Mouth::Frown; f.spin = 60;
      f.ov |= OV_AHEM | OV_CAPTION; f.captionColor = COL_YELLOW;
      snprintf(b, sizeof(b), "%.1fg", ctx.lon); brainSetCaption(b);
      break;
    case Expr::TurnLeft:  f.rdx = -10; f.tilt = -14; f.eyeDX = -14; f.mouth = Mouth::Small; f.ov |= OV_ARCS_R; break;
    case Expr::TurnRight: f.rdx = 10; f.tilt = 14; f.eyeDX = 14; f.mouth = Mouth::Small; f.ov |= OV_ARCS_L; break;
    case Expr::Accel:                         // design LAUNCH 0x02: determined, pumping hard
      f.spin = 1200; f.eyeDY = 2; f.brow = Brow::Determined; f.mouth = Mouth::Grit;
      f.ov |= OV_SIDELINES | OV_CAPTION; f.captionColor = COL_YELLOW;
      snprintf(b, sizeof(b), "+%.1fg", ctx.lon); brainSetCaption(b);
      break;
    case Expr::Bump:      f.rdy = -12; f.eye = Eye::Cross; f.eyeDY = -10; f.brow = Brow::Raised; f.mouth = Mouth::O; f.ov |= OV_JOLT; break;
    case Expr::Dizzy:     f.tilt = 24 * sinf(t * 2 * PI / 1.4f); f.eye = Eye::Spiral; f.brow = Brow::None; f.mouth = Mouth::Frown; f.ov |= OV_ORBIT; break;
    case Expr::V8:        f.spin = 1100; f.eye = Eye::Chevron; f.brow = Brow::Determined; f.mouth = Mouth::Grit; f.ov |= OV_BADGE42 | OV_SIDELINES; break;
    case Expr::Calibrate:
      f.ov = OV_RING | OV_HOUSING; f.dashed = true; f.rbright = 0.6f; f.rest = 1; f.eye = Eye::Closed; f.brow = Brow::None;
      f.ring = ctx.calibProgress;
      break;
    case Expr::Boot: {                        // wake-up: outline draws itself, a beat, eyes open, first strokes
      float el = (now - start) / 1000.0f;
      f.ov = OV_BOOT | OV_HOUSING; f.rdy = -10;
      f.drawFrac = fminf(1.0f, el / 1.5f);    // 0 - 1.5 s: the outline
      f.eyeBright = constrain((el - 1.9f) / 0.4f, 0.0f, 1.0f);   // 1.9 s: eyes fade in
      f.eye = Eye::Dot; f.brow = Brow::Raised; f.mouth = Mouth::Small;
      bool awake = el > 2.3f;                 // 2.3 s: rises off the bottom and starts to pump
      f.rest = awake ? 0 : 1; f.spin = awake ? 140 : 0;
      break;
    }
    case Expr::Saved:     f.eye = Eye::Happy; f.brow = Brow::Raised; f.mouth = Mouth::Pleased; f.ov |= OV_CHECK; brainSetCaption("SAVED"); break;
    case Expr::Charging:
      f.eye = Eye::Content; f.eyeDY = -4; f.mouth = Mouth::Pleased; f.spin = 60; f.ov |= OV_BATTERY | OV_CAPTION;
      f.captionColor = COL_YELLOW; f.batColor = COL_YELLOW; f.bat = ctx.batPct / 100.0f;
      snprintf(b, sizeof(b), "%d%%", ctx.batPct); brainSetCaption(b);
      break;
    case Expr::LowBattery:
      f.rbright = 0.55f; f.eye = Eye::Half; f.eyeDY = 4; f.brow = Brow::Sad; f.mouth = Mouth::Frown; f.spin = 20;
      f.ov |= OV_BATTERY | OV_CAPTION; f.batColor = COL_PINK; f.bat = fmaxf(0.06f, ctx.batPct / 100.0f);
      snprintf(b, sizeof(b), "%d%% - PLUG ME IN", ctx.batPct); brainSetCaption(b);
      break;
    case Expr::BatteryLevel: {               // briefly on every wake: how full am I?
      int p = ctx.batPct;
      f.eye = Eye::Dot; f.eyeDY = 4; f.eyeDX = 8; f.ov |= OV_BATTERY | OV_CAPTION;   // glances at the battery
      f.batColor = p > 50 ? COL_ICE : p > 25 ? COL_YELLOW : COL_PINK;
      f.captionColor = f.batColor; f.bat = fmaxf(0.06f, p / 100.0f);
      snprintf(b, sizeof(b), "%d%%", p); brainSetCaption(b);
      break;
    }
    case Expr::ChargeSoon:                   // ~25 %: a heads-up before the 15 % low-battery face
      f.eye = Eye::Half; f.eyeDY = 2; f.eyeDX = 8; f.mouth = Mouth::Small; f.ov |= OV_BATTERY | OV_CAPTION | OV_DOTS3;
      f.batColor = COL_YELLOW; f.captionColor = COL_YELLOW; f.bat = fmaxf(0.06f, ctx.batPct / 100.0f);
      snprintf(b, sizeof(b), "%d%% - CHARGE SOON", ctx.batPct); brainSetCaption(b);
      break;
    case Expr::Charged:                      // charging finished
      f.eye = Eye::Happy; f.brow = Brow::Raised; f.mouth = Mouth::Smile; f.ov |= OV_BATTERY | OV_CAPTION | OV_SPARKLE;
      f.spin = 300; f.batColor = COL_ICE; f.captionColor = COL_ICE; f.bat = 1.0f;
      brainSetCaption("FULL!");
      break;
    case Expr::Overheat:
      f.eye = Eye::Wavy; f.brow = Brow::Sad; f.mouth = Mouth::Frown; f.ov |= OV_SWEAT | OV_CAPTION; f.spin = 30;
      snprintf(b, sizeof(b), "TOO HOT %dC", (int)ctx.tempC); brainSetCaption(b);
      break;
    case Expr::Sleep:
      f.ov = OV_ZZ | OV_HOUSING; f.rbright = 0.35f; f.eyeBright = 0.3f; f.eye = Eye::Closed; f.brow = Brow::None;
      f.mouth = Mouth::Small; f.gray = true; f.rest = 1; f.spin = 0;
      break;
    case Expr::Greeting:
      f.ov = OV_GREETING | OV_SPEAK | OV_HOUSING; f.fcx = 120; f.fcy = 82; f.fscale = 0.5f;
      f.eye = s_greetEye; f.eyeR = 10; f.brow = Brow::Raised; f.mouth = Mouth::Talk;
      if (s_greetEye == Eye::Closed) { f.gray = true; f.rest = 1; f.mouth = Mouth::Small; f.ov |= OV_ZZ; f.ov &= ~OV_SPEAK; }
      if (s_greetEye == Eye::Chevron) f.brow = Brow::Determined;
      break;

    // ---------------------------------------------------------- touch ----
    case Expr::TapHappy: f.eye = Eye::Happy; f.brow = Brow::Raised; f.mouth = Mouth::Smile; f.spin = 300; f.ov |= OV_SPARKLE; break;
    case Expr::Spinning: f.eye = Eye::Happy; f.brow = Brow::Raised; f.mouth = Mouth::Smile; break;
    case Expr::Wobble: {
      float p = fminf(1.0f, (now - start) / 1600.0f);
      f.tilt = 12 * sinf(p * 2 * PI * 2.5f) * (1 - p); f.mouth = Mouth::O;
      break;
    }
    case Expr::Startled: f.rscale = 1.06f; f.eye = Eye::Wide; f.eyeR = 11; f.eyeDY = -2; f.brow = Brow::Raised; f.mouth = Mouth::O; f.ov |= OV_EXCLAIM; break;
    case Expr::Pet:                           // design PETTED 0x03: a small, dignified smile + heart
      f.eye = Eye::Happy; f.brow = Brow::Raised; f.mouth = Mouth::Smile; f.spin = 60; f.ov |= OV_HEARTS;
      f.rbright = 0.8f + 0.2f * cosf(t * 2 * PI / 2.4f);
      break;
    case Expr::Grumpy:
      f.eye = Eye::Half; f.eyeDY = 4; f.tilt = -6; f.brow = Brow::Angry; f.mouth = Mouth::Frown; f.spin = 80;
      f.ov |= OV_STEAM | OV_CAPTION; brainSetCaption("HEY!");
      break;
    case Expr::MenuOpening:
      f.ov = OV_OPENRING | OV_HOUSING; f.fscale = 0.6f; f.rbright = 0.6f; f.ring = s_ring; f.spin = 0;
      break;
    case Expr::MenuLocked:
      f.eye = Eye::Half; f.brow = Brow::Ahem; f.mouth = Mouth::Flat; f.spin = 60; f.ov |= OV_LOCK | OV_CAPTION;
      brainSetCaption("STOP FIRST");
      break;
    case Expr::SehrGut:                       // design SEHR GUT 0x05: approves of a smooth drive
      f.eye = Eye::Content; f.brow = Brow::Pleased; f.mouth = Mouth::Pleased; f.spin = 120;
      f.ov |= OV_TWINKLE | OV_CAPTION; f.captionColor = COL_YELLOW; brainSetCaption("SEHR GUT");
      break;

    // ----------------------------------------------------------- car -----
    case Expr::EngineOn:
      f.eye = Eye::Wide; f.eyeR = 10; f.brow = Brow::Raised; f.mouth = Mouth::O; f.spin = 600;
      f.rdx = 1.5f * sinf(t * 90); f.rdy = 1.0f * sinf(t * 77);   // rumble shake
      f.ov |= OV_RUMBLE | OV_CAPTION; f.captionColor = COL_YELLOW; brainSetCaption("ENGINE ON");
      break;
    case Expr::Rev:      f.spin = 1300; f.eye = Eye::Chevron; f.brow = Brow::Determined; f.mouth = Mouth::Grit; f.ov |= OV_PUFFS; break;
    // Drag race: mini game-face Otto left, tree right
    case Expr::LaunchReady:                   // pre-stage / staged
      f.ov = OV_TREE | OV_HOUSING | OV_BROWS_DET | OV_CAPTION; f.fcx = 82; f.fcy = 118; f.fscale = 0.5f; f.eyeR = 10;
      f.mouth = Mouth::Grit; f.captionColor = COL_YELLOW; f.spin = 200;   // caption set by launch.cpp
      break;
    case Expr::LaunchTree:                    // ambers counting down, the piston revs up
      f.ov = OV_TREE | OV_HOUSING | OV_BROWS_DET | OV_CAPTION; f.fcx = 82; f.fcy = 118; f.fscale = 0.5f; f.eyeR = 10;
      f.mouth = Mouth::Grit; f.captionColor = COL_GRAY; f.spin = 900;
      break;
    case Expr::LaunchGo:
      f.ov = OV_TREE | OV_HOUSING | OV_STREAKS; f.fcx = 86; f.fcy = 118; f.fscale = 0.5f; f.eye = Eye::Wide; f.eyeR = 11;
      f.brow = Brow::Determined; f.mouth = Mouth::O; f.spin = 1400;
      break;
    case Expr::LaunchRed:                     // left before green: wince + sweat drop
      f.ov = OV_TREE | OV_HOUSING | OV_CAPTION; f.fcx = 82; f.fcy = 118; f.fscale = 0.5f; f.eye = Eye::Chevron; f.eyeR = 11;
      f.brow = Brow::Sad; f.mouth = Mouth::Frown; f.captionColor = rgb565(255, 92, 122); f.spin = 0;
      break;
    case Expr::LaunchRecord:                  // new best: confetti + NEUER REKORD!
      f.ov = OV_GREETING | OV_HOUSING | OV_CONFETTI | OV_CAPTION; f.fcx = 120; f.fcy = 88; f.fscale = 0.52f;
      f.eye = Eye::Happy; f.eyeR = 10; f.brow = Brow::Raised; f.mouth = Mouth::Smile; f.captionColor = COL_ICE; f.spin = 600;
      break;
    case Expr::LaunchRun:
      f.ov = OV_ANSWER | OV_HOUSING | OV_SPEEDRING; f.fcx = 120; f.fcy = 66; f.fscale = 0.4f; f.eye = Eye::Chevron;
      f.brow = Brow::Determined; f.mouth = Mouth::Grit; f.spin = 1300; f.ring = s_ring;
      break;
    case Expr::LaunchResult:                  // timeslip: R/T, 0-30, 0-60, peak g + best
      f.ov = OV_SLIP | OV_CAPTION; f.rbright = 0; f.eyeBright = 0; f.captionColor = COL_PINK;
      break;
    case Expr::Summary:
      f.ov = OV_ANSWER | OV_HOUSING; f.fcx = 120; f.fcy = 70; f.fscale = 0.42f; f.eye = Eye::Content;
      f.brow = Brow::Pleased; f.mouth = Mouth::Pleased; f.eyeDY = -4;
      break;
    case Expr::Updating:
      f.ov = OV_UPDATE; f.fcy = 74; f.fscale = 0.3f; f.dashed = true; f.rbright = 0.6f; f.eyeBright = 0;
      f.spin = 200; f.ring = s_ring;
      break;
    case Expr::HotOff:
      f.eye = Eye::Wavy; f.brow = Brow::Sad; f.mouth = Mouth::Frown; f.rest = 1; f.spin = 0;
      f.ov |= OV_SWEAT | OV_CAPTION; brainSetCaption("TOO HOT - SLEEPING");
      break;

    // ----------------------------------------------------- holidays --------
    case Expr::HolidayBirthday:               // party hat + confetti, hopping
      f.ov = OV_GREETING | OV_HOUSING | OV_SPEAK | OV_PARTY_HAT | OV_CONFETTI;
      f.fcx = 120; f.fscale = 0.52f; f.eye = Eye::Happy; f.eyeR = 10; f.brow = Brow::Raised; f.mouth = Mouth::Smile;
      f.fcy = 90 - 5 * fabsf(sinf(t * 2 * PI / 0.5f));
      f.spin = 400;
      copyStr(s_txt.greet, sizeof(s_txt.greet), "Alles Gute!");
      copyStr(s_txt.gloss, sizeof(s_txt.gloss), "HAPPY BIRTHDAY!");
      break;
    case Expr::HolidayNewYear:
      f.ov = OV_GREETING | OV_HOUSING | OV_SPEAK | OV_CONFETTI | OV_FIREWORKS;
      f.fcx = 120; f.fcy = 88; f.fscale = 0.52f; f.eye = Eye::Happy; f.eyeR = 10; f.brow = Brow::Raised; f.mouth = Mouth::Smile;
      copyStr(s_txt.greet, sizeof(s_txt.greet), "Prost Neujahr!");
      copyStr(s_txt.gloss, sizeof(s_txt.gloss), "HAPPY NEW YEAR!");
      break;
    case Expr::HolidayJuly4:
      f.ov = OV_GREETING | OV_HOUSING | OV_SPEAK | OV_FIREWORKS;
      f.fcx = 120; f.fcy = 88; f.fscale = 0.52f; f.eye = Eye::Spiral; f.eyeR = 10; f.brow = Brow::Raised; f.mouth = Mouth::Smile;
      copyStr(s_txt.greet, sizeof(s_txt.greet), "HAPPY 4TH!");
      copyStr(s_txt.gloss, sizeof(s_txt.gloss), "JULY 4TH!");
      break;
    case Expr::HolidayHalloween:              // full-size jack-o'-lantern piston (OV_LANTERN)
      f.ov = OV_HOUSING | OV_LANTERN | OV_CAPTION;
      f.eyeBright = 0; f.rbright = 1; f.spin = 0;
      copyStr(s_txt.caption, sizeof(s_txt.caption), "TRICK OR TREAT");
      break;
    case Expr::HolidayChristmas:
      f.ov = OV_GREETING | OV_HOUSING | OV_SPEAK | OV_SANTA_HAT | OV_SNOW;
      f.fcx = 120; f.fcy = 88; f.fscale = 0.52f; f.eye = Eye::Happy; f.eyeR = 10; f.brow = Brow::Raised; f.mouth = Mouth::Smile;
      copyStr(s_txt.greet, sizeof(s_txt.greet), "Frohe Weihnachten!");
      copyStr(s_txt.gloss, sizeof(s_txt.gloss), "MERRY CHRISTMAS");
      break;

    // ------------------------------------------------ everyday extras ----
    case Expr::Guard:                         // someone bumped the car while parked
      f.ov = OV_ANSWER | OV_HOUSING | OV_IMPACT; f.fcx = 120; f.fcy = 66; f.fscale = 0.42f;
      f.eye = Eye::Ahem; f.brow = Brow::Ahem; f.mouth = Mouth::Frown; f.eyeR = 10;
      break;
    case Expr::MissedYou:                     // parked 7+ days: Lange nicht gesehen!
      f.ov = OV_GREETING | OV_HOUSING | OV_HEARTS | OV_CAPTION; f.fcx = 120; f.fcy = 80; f.fscale = 0.5f;
      f.eye = Eye::Happy; f.eyeR = 10; f.brow = Brow::Raised; f.mouth = Mouth::Smile; f.captionColor = COL_YELLOW;
      break;
    case Expr::Milestone:                     // drive #100: dotted gold ring + sparkles
      f.ov = OV_ANSWER | OV_HOUSING | OV_DOTRING | OV_SPARKLE; f.fcx = 120; f.fcy = 62; f.fscale = 0.38f;
      f.eye = Eye::Happy; f.eyeR = 10; f.brow = Brow::Raised; f.mouth = Mouth::Smile; f.spin = 300;
      break;
    case Expr::BreakTime:                     // Pause? with a steaming cup
      f.ov = OV_GREETING | OV_HOUSING | OV_CUP | OV_CAPTION; f.fcx = 112; f.fcy = 80; f.fscale = 0.5f;
      f.eye = Eye::Content; f.eyeR = 10; f.brow = Brow::Pleased; f.mouth = Mouth::Pleased; f.captionColor = COL_YELLOW; f.spin = 50;
      break;
    case Expr::HotCabin:                      // Heiß! under heat waves
      f.ov = OV_GREETING | OV_HOUSING | OV_HEAT | OV_CAPTION; f.fcx = 120; f.fcy = 92; f.fscale = 0.5f;
      f.eye = Eye::Half; f.eyeR = 10; f.eyeDY = 6; f.brow = Brow::Sad; f.mouth = Mouth::O; f.captionColor = COL_ICE; f.spin = 30;
      break;
    case Expr::ColdCabin:                     // Kalt! shivering under snow
      f.ov = OV_GREETING | OV_HOUSING | OV_SNOW | OV_CAPTION; f.fcx = 120 + 2 * sinf(t * 2 * PI * 7); f.fcy = 88;
      f.fscale = 0.5f; f.eye = Eye::Dot; f.eyeR = 8; f.brow = Brow::Sad; f.mouth = Mouth::Grit; f.captionColor = COL_YELLOW; f.spin = 0;
      break;
    case Expr::ShowCard:                      // car show mode: star eyes + spec card
      f.ov = OV_CARD | OV_HOUSING | OV_SPARKLE; f.fcx = 120; f.fcy = 60; f.fscale = 0.38f; f.eye = Eye::Spiral; f.eyeR = 10;
      f.brow = Brow::Raised; f.mouth = Mouth::Smile;
      break;
    case Expr::HolidayPatent:                 // Jan 29: Benz's 1886 patent, the car's birthday; proud in a wreath
      f.ov = OV_WREATH | OV_HOUSING | OV_SPEAK; f.fcx = 120; f.fcy = 96; f.fscale = 0.52f; f.eye = Eye::Content; f.eyeR = 10;
      f.brow = Brow::Pleased; f.mouth = Mouth::Pleased;
      break;

    // --------------------------------------------------------- OBD-II ----
    case Expr::WarmUp: {                      // cold engine after a start: go easy
      f.eye = Eye::Half; f.brow = Brow::Sad; f.mouth = Mouth::Grit; f.rdx = 0.8f * sinf(t * 2 * PI * 7);
      f.ov |= OV_THERMO | OV_CAPTION; f.bat = 0.15f; f.batColor = COL_ICE; f.captionColor = COL_YELLOW;
      if (ctx.coolantC > -100) snprintf(b, sizeof(b), "NOCH KALT %dC", (int)ctx.coolantC); else snprintf(b, sizeof(b), "NOCH KALT");
      brainSetCaption(b);
      break;
    }
    case Expr::Warm:                          // reached operating temperature
      f.eye = Eye::Content; f.brow = Brow::Pleased; f.mouth = Mouth::Pleased;
      f.ov |= OV_THERMO | OV_CAPTION; f.bat = 0.6f; f.batColor = COL_ICE; f.captionColor = COL_ICE;
      snprintf(b, sizeof(b), "WARM %dC", (int)ctx.coolantC); brainSetCaption(b);
      break;
    case Expr::EngineHot:                     // the car is running hot (not Otto)
      f.eye = Eye::Wavy; f.brow = Brow::Sad; f.mouth = Mouth::Frown;
      f.ov |= OV_THERMO | OV_SWEAT | OV_CAPTION; f.bat = 1.0f; f.batColor = COL_PINK; f.captionColor = COL_PINK;
      snprintf(b, sizeof(b), "ENGINE %dC!", (int)ctx.coolantC); brainSetCaption(b);
      break;
    case Expr::CheckEngine:                   // card: code + "CODE 1 OF 2" (set by obd.cpp)
      f.ov = OV_ANSWER | OV_HOUSING; f.fcx = 120; f.fcy = 66; f.fscale = 0.42f;
      f.eye = Eye::Ahem; f.brow = Brow::Ahem; f.mouth = Mouth::Frown; f.eyeR = 10;
      break;

    case Expr::WifiTune:                      // card: WI-FI OTTO-xxxx / password / address (set by wifitune.cpp)
      f.ov = OV_ANSWER | OV_HOUSING; f.fcx = 120; f.fcy = 66; f.fscale = 0.42f;
      f.eye = Eye::Dot; f.brow = Brow::Raised; f.mouth = Mouth::Pleased; f.eyeR = 10;
      break;

    default: break;
  }

  // All-day costumes: add the hat overlay to any expression after the first-wake greeting
  if (holidayCostumeOn()) {
    Holiday hc = holidayCostumeType();
    bool fullscreen = (e == Expr::Boot || e == Expr::Sleep || e == Expr::Calibrate ||
                       e == Expr::Updating || e == Expr::Volume || e == Expr::LaunchResult ||
                       e == Expr::HolidayBirthday || e == Expr::HolidayNewYear ||
                       e == Expr::HolidayJuly4    || e == Expr::HolidayHalloween ||
                       e == Expr::HolidayChristmas || e == Expr::HolidayPatent);
    if (!fullscreen) {
      if (hc == Holiday::Birthday || hc == Holiday::NewYear) f.ov |= OV_PARTY_HAT;
      else if (hc == Holiday::Christmas || hc == Holiday::ChristmasEve) f.ov |= OV_SANTA_HAT;
      else if (hc == Holiday::PatentDay) f.ov |= OV_WREATH_ALLDAY;
      if ((hc == Holiday::Birthday || hc == Holiday::NewYear) && e == Expr::Braking)
        f.rdy -= 6 * fabsf(sinf(t * 2 * PI / 0.4f));  // hat hops on braking
    }
  }
}

void brainUpdate(uint32_t now, const BrainCtx& ctx, FaceState& target, float& blinkOpen) {
  s_now = now;
  if (s_voice != Expr::Idle && (int32_t)(now - s_voiceUntil) >= 0) s_voice = Expr::Idle;
  bool evOn = s_ev.on && (int32_t)(now - s_ev.until) < 0;
  if (!evOn) s_ev.on = false;

  // highest priority wins: voice (70) or the one-shot event (its own priority)
  Expr e = Expr::Idle;
  uint32_t start = s_curStart;
  int best = -1;
  if (s_voice != Expr::Idle) { best = VOICE_PRIO; e = s_voice; }
  if (evOn && s_ev.prio > best) { best = s_ev.prio; e = s_ev.e; start = s_ev.start; }

  if (best >= 0) {
    // chosen above
  } else if (ctx.playing && g_set.bobOn) {
    e = Expr::MusicBob; start = s_curStart;
  } else if (g_set.boredOn && ctx.hadDriven && ctx.stoppedSecs > 45 && ctx.stoppedSecs < 900) {
    e = Expr::Bored; start = s_curStart;
  } else {
    e = Expr::Idle; start = s_curStart;
  }
  if (e != s_cur) { s_cur = e; s_curStart = now; if (start == 0) start = now; }

  build(e, now, start, ctx, target);

  bool idleish = (e == Expr::Idle || e == Expr::Bored || e == Expr::MusicBob);
  updateMicro(now, ctx);
  if (e == Expr::Idle) applyMicro(now, target);
  if (!idleish && s_micro != Micro::None) s_micro = Micro::None;  // real events cancel idle bits

  blinkOpen = updateBlink(now);

  // OBD: the piston pumps with the real engine (scaled: 700 rpm idle -> the design's 140 deg/s pump,
  // 6000 rpm -> 1200), on full-size faces that are pumping anyway. Shift light at the set rpm.
  if (ctx.rpm > 300 && target.rest < 0.5f && target.spin > 0 && target.fscale > 0.99f && !(target.ov & OV_LANTERN))
    target.spin = ctx.rpm * 0.2f;
  if (g_set.shiftRpm > 0 && ctx.rpm >= g_set.shiftRpm && !menuIsOpen()) {
    target.ov |= OV_SHIFT;
    if (target.fscale > 0.99f) target.brow = Brow::Angry;
  }

}
