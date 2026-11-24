#include "music.h"
#include "settings.h"
#include "audio.h"
#include "ble.h"
#include "motion.h"
#include <math.h>

// ---- feed (runs on whichever task reads the mic: the ESP-SR feed task) ----
// Every 512 samples (32 ms at 16 kHz) store two levels in a ring: the bass (< ~150 Hz, kick drum and
// bass line) and the whole band. Nothing heavier than a few multiplies per sample happens here.
static const int FRAME = 512;
static const float FPS = 16000.0f / FRAME;      // 31.25 frames per second
static const int N = 192;                       // ~6.1 s of history
static float s_lb[N], s_la[N];                  // dBFS per frame: bass, all
static volatile uint32_t s_frames = 0;
static float s_dc = 0, s_lp = 0, s_eb = 0, s_ea = 0;
static int s_cnt = 0;

void musicFeed(const int16_t* s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    float x = s[i];
    s_dc += (x - s_dc) * (1.0f / 512);           // remove the mic's DC offset
    x -= s_dc;
    s_lp += (x - s_lp) * 0.06f;                  // one-pole low-pass ~150 Hz
    s_eb += s_lp * s_lp;
    s_ea += x * x;
    if (++s_cnt == FRAME) {
      uint32_t f = s_frames;
      s_lb[f % N] = 10 * log10f(s_eb / FRAME + 1) - 90.3f;
      s_la[f % N] = 10 * log10f(s_ea / FRAME + 1) - 90.3f;
      s_frames = f + 1;
      s_cnt = 0; s_eb = 0; s_ea = 0;
    }
  }
}

// ---- analysis (main loop, twice a second) ----
// Tunable on the bench with `music loud 6` / `music beat 0.2` / `music min -52` (not saved).
// Loud = the last 2 s are this far above the background (learned: the quietest it's been lately).
// Bench, Nov 2026: quiet room -60.5 dB, phone music at 30 cm -51.5, talking -36..-48 but gappy.
static float s_loudDb = 6;        // dB above the background
static float s_steadyMin = 0.85f; // share of the last 4 s that isn't a gap (speech has gaps, music doesn't)
static float s_beatMin = 0.20f;   // autocorrelation peak of the bass kicks at the beat period
// Absolute floor (dBFS): a silent office read -55.8 dB with a faint ~157 BPM tick (fan / hum) and the
// background learned at -61.7, so it passed "+6 dB" and the looser keep limits held the bob on with
// no music at all (owner, Nov 22 2026). Starting needs > s_minDb, staying > s_minDb - 3. Phone music
// at 30 cm read -51.5 on the bench; car speakers should be far louder (check `drivelog` lvl).
static float s_minDb = -52;
static const int LAG_MIN = 9, LAG_MAX = 34;   // beat strength: any period from 208 .. 55 BPM
// Tempo for the bob: only 82-170 BPM. Bench: a 118 BPM song's kick lands every other beat and read
// as 59 BPM when any period was allowed.
static const int TEMPO_MIN = 11, TEMPO_MAX = 23;
static float s_floor = -62;       // background level (dBFS); starts as a quiet room so music already
                                  // playing at power-up still counts (bench: it was)

static bool s_heard = false;
static bool s_live = false;       // analysis running on fresh mic data

// The car's own noise, learned by ENGINE LEARN (idle) / learn the road (cruising), with music off.
// While the engine runs, the background never sits below it: no 30 s catch-up after a wake, and a
// quiet moment can't make the car look loud. -999 = nothing learned (or engine off).
static const float LEARNED_SLACK_DB = 6;   // see analyse(): how far below the learned noise it may sit
static bool engineIdling() { return g_motion.engineOn && g_motion.quietSecs > 1; }
static float learnedNoise() {
  if (!g_motion.engineOn) return -999;
  float idle = g_set.idleNoiseDb < 0 ? g_set.idleNoiseDb : -999;
  float cruise = g_set.cruiseNoiseDb < 0 ? g_set.cruiseNoiseDb : -999;
  return engineIdling() ? idle : fmaxf(idle, cruise);
}
// The V8's idle rhythm reads as a beat: at idle, a tempo near the learned one (or half / double) isn't music.
static bool engineTempo(float bpm) {
  if (!engineIdling() || g_set.idleBpm <= 0) return false;
  for (float m : {1.0f, 2.0f, 0.5f}) if (fabsf(bpm - g_set.idleBpm * m) < g_set.idleBpm * m * 0.06f) return true;
  return false;
}
static float s_bpm = 120, s_bpmShown = 120;
static int s_yes = 0, s_no = 0;
static uint32_t s_lastRun = 0, s_lastFrames = 0, s_audioAt = 0, s_watchUntil = 0;
static float s_lvl = -99, s_steady = 0, s_beat = 0, s_est = 0;
static bool s_cand = false;

bool musicHeard() { return s_heard; }
void musicDebug(float& lvl, float& floor, float& beat) { lvl = s_lvl; floor = s_floor; beat = s_beat; }
float musicBpm() { return s_bpmShown; }
float musicSteady() { return s_steady; }

static float pctile(float* v, int n, float p) {     // sorts v in place
  for (int i = 1; i < n; i++) { float t = v[i]; int j = i - 1; while (j >= 0 && v[j] > t) { v[j + 1] = v[j]; j--; } v[j + 1] = t; }
  return v[constrain((int)(p * (n - 1) + 0.5f), 0, n - 1)];
}

static void setHeard(bool on) {
  if (on == s_heard) return;
  s_heard = on;
  if (on) { s_bpmShown = s_bpm = s_est; Serial.printf("music: heard, ~%.0f BPM\n", s_bpm); }
  else Serial.println("music: stopped");
}

static void analyse() {
  static float lb[N], la[N], o[N], tmp[N];
  uint32_t f = s_frames;
  for (int k = 0; k < N; k++) { lb[k] = s_lb[(f - N + k) % N]; la[k] = s_la[(f - N + k) % N]; }

  // loud: median of the last 2 s
  const int L2 = 64, L4 = 128;
  memcpy(tmp, la + N - L2, L2 * sizeof(float));
  s_lvl = pctile(tmp, L2, 0.5f);
  // steady: frames of the last 4 s within 12 dB of the loud part
  memcpy(tmp, la + N - L4, L4 * sizeof(float));
  float top = pctile(tmp, L4, 0.8f);
  int ok = 0;
  for (int k = N - L4; k < N; k++) ok += la[k] >= top - 12;
  s_steady = (float)ok / L4;

  // beat: onset strength = level rises (bass counts double), then autocorrelation over 6 s
  o[0] = 0;
  float mean = 0;
  for (int k = 1; k < N; k++) {
    o[k] = fmaxf(0, lb[k] - lb[k - 1]) + 0.5f * fmaxf(0, la[k] - la[k - 1]);
    mean += o[k];
  }
  mean /= N - 1;
  float e = 0;
  for (int k = 1; k < N; k++) { o[k] -= mean; e += o[k] * o[k]; }
  o[0] = 0;
  float r[LAG_MAX + 2] = {0};
  int best = 0;
  if (e > 1e-3f) {
    for (int L = LAG_MIN - 1; L <= LAG_MAX + 1; L++) {
      float acc = 0;
      for (int k = L + 1; k < N; k++) acc += o[k] * o[k - L];
      r[L] = acc / e;
    }
    for (int L = LAG_MIN; L <= LAG_MAX; L++) if (!best || r[L] > r[best]) best = L;
  }
  s_beat = best ? r[best] : 0;
  if (best) {
    best = 0;
    for (int L = TEMPO_MIN; L <= TEMPO_MAX; L++) if (!best || r[L] > r[best]) best = L;
    float a = r[best - 1], b = r[best], c = r[best + 1];       // parabolic peak for a finer lag
    float d = a - 2 * b + c, lag = best + (fabsf(d) > 1e-6f ? 0.5f * (a - c) / d : 0);
    s_est = 60.0f * FPS / lag;
  }
  // background: drops to any quieter 2 s at once, creeps up ~2.4 dB a minute (road noise rising
  // with speed), but not while music is heard or it would learn the music as background
  if (s_lvl < s_floor) s_floor = s_lvl;
  // Loud with no music (the V8 idling at -22 dB, road noise -10..-25) pulls it up fast, 1 dB/s: at
  // 2.4 dB/min a quiet moment at boot (-58) left everything in the car "6 dB louder", and idle rumble
  // bobbed him (drive log, Nov 22 2026). Music starts the bob within ~2 s, so it costs ~2 dB.
  else if (!s_heard) s_floor += s_lvl > s_floor + s_loudDb ? 0.5f : 0.02f;
  // The learned noise holds the background up, with room for a quieter cabin: the Civic learned
  // -15 dB cruising with the defroster on, and a floor stuck there would leave quieter music unheard
  // for good once the fan is off (owner, Nov 23 2026). The 1 dB/s rise above still catches up.
  float car = learnedNoise();
  if (car > -999 && s_floor < car - LEARNED_SLACK_DB) s_floor = car - LEARNED_SLACK_DB;
  // Hysteresis: starting needs the full test, but once bobbing, looser limits keep it going through
  // quieter bars and road noise (in the car the bob dropped out on its own, Nov 19 2026: road
  // rumble sits right in the bass band the beat comes from).
  if (s_heard) s_cand = s_lvl > s_floor + s_loudDb * 0.5f && s_lvl > s_minDb - 3 && s_steady >= s_steadyMin - 0.15f && s_beat >= s_beatMin * 0.6f;
  else s_cand = s_lvl > s_floor + s_loudDb && s_lvl > s_minDb && !engineTempo(s_est) && s_steady >= s_steadyMin && s_beat >= s_beatMin;
}

void musicUpdate(uint32_t now) {
  if (audioBusy()) s_audioAt = now;
  if (now - s_lastRun < 500) return;
  s_lastRun = now;
  uint32_t f = s_frames;
  bool fresh = f != s_lastFrames;
  s_lastFrames = f;
  s_live = g_set.bobOn && fresh && f >= (uint32_t)N;
  if (!s_live) {                                      // off, or mic not read
    s_yes = s_no = 0;
    setHeard(false);
    return;
  }
  analyse();
  // Otto's own speaker is in the window: hold the current state rather than judge it
  bool selfNoise = s_audioAt && now - s_audioAt < (uint32_t)(N / FPS * 1000);
  if (!selfNoise) {
    // Starting also needs a steady tempo: 4 checks in a row (~2 s) within 8 % of each other. Room
    // noise passes "steady" easily and a faint ~174 BPM tick sat just under the beat limit, so a TV /
    // talking / a fan pushing the level up gave false bobbing; noise's tempo jumps around (99, 104,
    // 94, 173 ... BPM), music's doesn't (owner, Nov 21 2026).
    static float s_candBpm = 0;
    if (s_cand) {
      s_no = 0;
      if (s_heard) s_yes++;
      else if (s_yes && fabsf(s_est - s_candBpm) < s_candBpm * 0.08f) s_yes++;
      else { s_yes = 1; s_candBpm = s_est; }
      // not in the first 30 s after a wake: the background is still catching up with the engine
      if (s_yes >= 4 && (now > 30000 || learnedNoise() > -999)) setHeard(true);   // learned: no wait
    }
    else { s_yes = 0; if (++s_no >= 16) setHeard(false); }          // 8 s without it (song gap ends it)
    // Staying on needs the tempo too: of the last 16 checks (8 s), at least 5 within 8 % of the bob's
    // tempo (or half / double it). An office with a fan / PC / typing passed the loose keep limits
    // forever while its tempo hopped 81 -> 105 -> 118 -> 134 -> 157 BPM (owner, Nov 22 2026).
    static uint16_t s_onTempo = 0;                                   // one bit per check, newest = bit 0
    static uint8_t s_tempoN = 0;
    if (!s_heard) { s_onTempo = 0; s_tempoN = 0; }
    else {
      auto near = [](float a, float b) { return fabsf(a - b) < b * 0.08f; };
      bool on = s_cand && (near(s_est, s_bpm) || near(s_est, s_bpm * 2) || near(s_est, s_bpm * 0.5f));
      s_onTempo = (uint16_t)((s_onTempo << 1) | (on ? 1 : 0));
      if (s_tempoN < 16) s_tempoN++;
      if (s_tempoN >= 16 && __builtin_popcount(s_onTempo) < 5) {
        setHeard(false);
        Serial.println("music: tempo wandered, stopped");
      }
    }
    if (s_heard && s_cand) {                                         // follow the tempo, slowly
      if (fabsf(s_est - s_bpm) < s_bpm * 0.15f) s_bpm += (s_est - s_bpm) * 0.3f;
      // only move the bob's tempo on a real change: a jump in BPM jumps the bob's phase
      if (fabsf(s_bpm - s_bpmShown) > s_bpmShown * 0.06f) s_bpmShown = s_bpm;
    }
  }
  if (s_watchUntil && (int32_t)(now - s_watchUntil) < 0)
    Serial.printf("music: level %.1f dB (bg %.1f)  steady %.2f  beat %.2f  ~%.0f BPM  %s%s%s\n", s_lvl, s_floor, s_steady, s_beat,
                  s_est, s_cand ? "looks like music" : "no", selfNoise ? " (own speaker, holding)" : "",
                  s_heard ? "  [BOBBING]" : "");
}

bool musicLive() { return s_live; }
void musicSample(float& lvl, float& beat, float& bpm) { lvl = s_lvl; beat = s_beat; bpm = s_est; }

void musicStatus(char* b, size_t n) {
  snprintf(b, n, "music: %s (%.0f BPM); level %.1f dB (background %.1f, needs +%.0f and > %.0f), steady %.2f (min %.2f), beat %.2f (min %.2f), est %.0f BPM%s",
           s_heard ? "HEARD" : "not heard", s_bpmShown, s_lvl, s_floor, s_loudDb, s_minDb, s_steady, s_steadyMin, s_beat, s_beatMin, s_est,
           g_set.bobOn ? "" : "; MUSIC BOB is off");
}

void musicWatch(float secs) {
  if (secs <= 0 || secs > 600) secs = 30;
  s_watchUntil = millis() + (uint32_t)(secs * 1000);
}

void musicSetThresholds(const char* what, float v) {
  if (!strcasecmp(what, "loud")) s_loudDb = v;
  else if (!strcasecmp(what, "beat")) s_beatMin = v;
  else if (!strcasecmp(what, "steady")) s_steadyMin = v;
  else if (!strcasecmp(what, "min")) s_minDb = v;
}
