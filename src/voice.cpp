#include "voice.h"
#include "mic.h"
#include "brain.h"
#include "audio.h"
#include "settings.h"
#include "motion.h"
#include "menu.h"
#include "launch.h"
#include "trip.h"
#include "board.h"
#include "extras.h"
#include "commands.h"
#include "pwrtest.h"
#include "wifitune.h"
#include <ESP_SR.h>
#include <esp_partition.h>

void requestSleep();  // main.cpp

// Phonemes generated with the Arduino ESP_SR tool (needs `pip install g2p_en`):
//   python gen_sr_commands.py "lets drag race,drag race,launch mode;open menu,show menu;..."
// Same id = same action; several phrasings each.
enum VoiceCmd {
  VC_DRAG_RACE, VC_MENU, VC_BRIGHTER, VC_DIMMER, VC_LOUDER, VC_QUIETER, VC_MUTE, VC_UNMUTE,
  VC_BEST, VC_SUMMARY, VC_SLEEP, VC_TIME, VC_SHOW, VC_SHOW_OFF, VC_CALIBRATE,
  VC_LEARN_ROAD, VC_LEARN_ENGINE,
  VC_VOLUME,             // VC_VOLUME + i = set the volume to kVolLevels[i]
  VC_BRIGHT = VC_VOLUME + 20,   // VC_BRIGHT + i = set the brightness to kBriLevels[i]
};
static const int kBriLevels[] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
static const int kVolLevels[] = {10, 15, 20, 25, 30, 40, 50, 60, 70, 80, 90, 100};
static const sr_cmd_t kCmds[] = {
  {VC_DRAG_RACE, "lets drag race", "LfTS DRaG RdS"},
  {VC_DRAG_RACE, "drag race", "DRaG RdS"},
  {VC_DRAG_RACE, "launch mode", "LeNp MbD"},
  {VC_MENU, "open menu", "bPcN MfNYo"},
  {VC_MENU, "show menu", "sb MfNYo"},
  {VC_BRIGHTER, "brightness up", "BRiTNcS cP"},
  {VC_BRIGHTER, "brighter", "BRiTk"},
  {VC_DIMMER, "brightness down", "BRiTNcS DtN"},
  {VC_DIMMER, "dimmer", "DgMk"},
  {VC_LOUDER, "volume up", "VnLYoM cP"},
  {VC_LOUDER, "louder", "LtDk"},
  {VC_QUIETER, "volume down", "VnLYoM DtN"},
  {VC_QUIETER, "quieter", "KWicTk"},
  {VC_MUTE, "mute sound", "MYoT StND"},
  {VC_MUTE, "be quiet", "Bm KWicT"},
  {VC_UNMUTE, "unmute", "cNMYoT"},
  {VC_UNMUTE, "sound on", "StND nN"},
  {VC_BEST, "best time", "BfST TiM"},
  {VC_BEST, "show best time", "sb BfST TiM"},
  {VC_SUMMARY, "trip summary", "TRgP ScMkm"},
  {VC_SLEEP, "go to sleep", "Gb To SLmP"},
  {VC_SLEEP, "good night", "GwD NiT"},
  {VC_TIME, "what time is it", "WcT TiM gZ gT"},
  {VC_TIME, "show the time", "sb jc TiM"},
  {VC_SHOW, "car show mode", "KnR sb MbD"},
  {VC_SHOW, "show mode", "sb MbD"},
  {VC_SHOW_OFF, "stop show", "STnP sb"},
  {VC_SHOW_OFF, "end show mode", "fND sb MbD"},
  {VC_CALIBRATE, "recalibrate", "RmKaLcBRdT"},
  {VC_CALIBRATE, "calibrate", "KaLcBRdT"},
  {VC_CALIBRATE, "recalibrate mount", "RmKaLcBRdT MtNT"},
  {VC_CALIBRATE, "level yourself", "LfVcL YkSfLF"},
  // hand-encoded with the tool's alphabet (ER = k, DH = j, OW = b, NG = l, JH = q; "turn" = TkN)
  {VC_LEARN_ROAD, "learn the road", "LkN jc RbD"},
  {VC_LEARN_ROAD, "learn driving", "LkN DRiVgl"},
  {VC_LEARN_ENGINE, "learn the engine", "LkN jc fNqcN"},
  {VC_LEARN_ENGINE, "engine learn", "fNqcN LkN"},
  {VC_VOLUME + 0, "volume ten", "VnLYoM TfN"},
  {VC_VOLUME + 0, "volume ten percent", "VnLYoM TfN PkSfNT"},
  {VC_VOLUME + 1, "volume fifteen", "VnLYoM FgFTmN"},
  {VC_VOLUME + 1, "volume fifteen percent", "VnLYoM FgFTmN PkSfNT"},
  {VC_VOLUME + 2, "volume twenty", "VnLYoM TWfNTm"},
  {VC_VOLUME + 2, "volume twenty percent", "VnLYoM TWfNTm PkSfNT"},
  {VC_VOLUME + 3, "volume twenty five", "VnLYoM TWfNTm FiV"},
  {VC_VOLUME + 3, "volume twenty five percent", "VnLYoM TWfNTm FiV PkSfNT"},
  {VC_VOLUME + 4, "volume thirty", "VnLYoM vkDm"},
  {VC_VOLUME + 4, "volume thirty percent", "VnLYoM vkDm PkSfNT"},
  {VC_VOLUME + 5, "volume forty", "VnLYoM FeRTm"},
  {VC_VOLUME + 5, "volume forty percent", "VnLYoM FeRTm PkSfNT"},
  {VC_VOLUME + 6, "volume fifty", "VnLYoM FgFTm"},
  {VC_VOLUME + 6, "volume fifty percent", "VnLYoM FgFTm PkSfNT"},
  {VC_VOLUME + 6, "volume half", "VnLYoM haF"},
  {VC_VOLUME + 7, "volume sixty", "VnLYoM SgKSTm"},
  {VC_VOLUME + 7, "volume sixty percent", "VnLYoM SgKSTm PkSfNT"},
  {VC_VOLUME + 8, "volume seventy", "VnLYoM SfVcNTm"},
  {VC_VOLUME + 8, "volume seventy percent", "VnLYoM SfVcNTm PkSfNT"},
  {VC_VOLUME + 9, "volume eighty", "VnLYoM dTm"},
  {VC_VOLUME + 9, "volume eighty percent", "VnLYoM dTm PkSfNT"},
  {VC_VOLUME + 10, "volume ninety", "VnLYoM NiNTm"},
  {VC_VOLUME + 10, "volume ninety percent", "VnLYoM NiNTm PkSfNT"},
  {VC_VOLUME + 11, "volume one hundred", "VnLYoM WcN hcNDRcD"},
  {VC_VOLUME + 11, "volume full", "VnLYoM FwL"},
  {VC_VOLUME + 11, "max volume", "MaKS VnLYoM"},
  {VC_VOLUME + 1, "volume low", "VnLYoM Lb"},          // 15 %  (word levels: the model is much
  {VC_VOLUME + 5, "volume medium", "VnLYoM MmDmcM"},   // 40 %   surer of these than of numbers)
  {VC_VOLUME + 5, "volume normal", "VnLYoM NeRMcL"},   // 40 % ("volume medium" kept coming back as
  {VC_VOLUME + 5, "normal volume", "NeRMcL VnLYoM"},   //  "volume up" on the bench)
  {VC_VOLUME + 5, "medium volume", "MmDmcM VnLYoM"},
  {VC_VOLUME + 8, "volume high", "VnLYoM hi"},         // 70 %
  {VC_BRIGHT + 1, "brightness low", "BRiTNcS Lb"},     // 20 %
  {VC_BRIGHT + 4, "brightness medium", "BRiTNcS MmDmcM"},  // 50 %
  {VC_BRIGHT + 7, "brightness high", "BRiTNcS hi"},    // 80 %
  {VC_BRIGHT + 0, "brightness ten", "BRiTNcS TfN"},
  {VC_BRIGHT + 0, "brightness ten percent", "BRiTNcS TfN PkSfNT"},
  {VC_BRIGHT + 1, "brightness twenty", "BRiTNcS TWfNTm"},
  {VC_BRIGHT + 1, "brightness twenty percent", "BRiTNcS TWfNTm PkSfNT"},
  {VC_BRIGHT + 2, "brightness thirty", "BRiTNcS vkDm"},
  {VC_BRIGHT + 2, "brightness thirty percent", "BRiTNcS vkDm PkSfNT"},
  {VC_BRIGHT + 3, "brightness forty", "BRiTNcS FeRTm"},
  {VC_BRIGHT + 3, "brightness forty percent", "BRiTNcS FeRTm PkSfNT"},
  {VC_BRIGHT + 4, "brightness fifty", "BRiTNcS FgFTm"},
  {VC_BRIGHT + 4, "brightness fifty percent", "BRiTNcS FgFTm PkSfNT"},
  {VC_BRIGHT + 4, "brightness half", "BRiTNcS haF"},
  {VC_BRIGHT + 5, "brightness sixty", "BRiTNcS SgKSTm"},
  {VC_BRIGHT + 5, "brightness sixty percent", "BRiTNcS SgKSTm PkSfNT"},
  {VC_BRIGHT + 6, "brightness seventy", "BRiTNcS SfVcNTm"},
  {VC_BRIGHT + 6, "brightness seventy percent", "BRiTNcS SfVcNTm PkSfNT"},
  {VC_BRIGHT + 7, "brightness eighty", "BRiTNcS dTm"},
  {VC_BRIGHT + 7, "brightness eighty percent", "BRiTNcS dTm PkSfNT"},
  {VC_BRIGHT + 8, "brightness ninety", "BRiTNcS NiNTm"},
  {VC_BRIGHT + 8, "brightness ninety percent", "BRiTNcS NiNTm PkSfNT"},
  {VC_BRIGHT + 9, "brightness one hundred", "BRiTNcS WcN hcNDRcD"},
  {VC_BRIGHT + 9, "brightness full", "BRiTNcS FwL"},
  {VC_BRIGHT + 9, "max brightness", "MaKS BRiTNcS"},
};
static const int N_CMDS = sizeof(kCmds) / sizeof(kCmds[0]);

// The recogniser hands us 16-bit mono; the mic sends 24-bit samples in 32-bit stereo slots.
// This stream converts on the fly (ESP_SR reads it through I2SClass::readBytes, which is virtual).
class MicStream : public I2SClass {
public:
  size_t readBytes(char* buffer, size_t size) override {
    return micRead((int16_t*)buffer, size / 2) * 2;
  }
};
static MicStream s_stream;

static bool s_running = false, s_modelsOk = false;
static volatile int s_event = -1;      // 0 wake, 1 command, 2 timeout
static volatile int s_cmd = -1, s_phrase = -1;
static int s_lastVol = 60;
static int s_misses = 0;
static uint32_t s_listenSince = 0;
static uint32_t s_calAt = 0;           // "recalibrate": start a moment later, once the cabin is quiet
// Ignore guesses the model is less sure of than this (bench, Nov 2026: "volume fifty" came back as
// "volume up" at 0.06 sure). Change with `listen thresh 0.x`; not saved.
static float s_thresh = 0.15f;

static void onSr(sr_event_t ev, int cmd, int phrase) {
  // Runs on the recogniser's task: just note it, the main loop acts on it.
  switch (ev) {
    case SR_EVENT_WAKEWORD: s_event = 0; break;
    case SR_EVENT_WAKEWORD_CHANNEL: ESP_SR.setMode(SR_MODE_COMMAND); break;
    case SR_EVENT_COMMAND: s_cmd = cmd; s_phrase = phrase; s_event = 1; ESP_SR.setMode(SR_MODE_WAKEWORD); break;
    case SR_EVENT_TIMEOUT: s_event = 2; ESP_SR.setMode(SR_MODE_WAKEWORD); break;
    default: break;
  }
}

// ---- listening window (owner, Nov 19 2026: voice is ~1/6 of the savable draw) ----
// "Hi ESP" listens for VOICE_WINDOW_MS after a wake, an engine start, a touch or the menu, and
// after each use; then the recogniser naps until the next one. VOICE off = never.
static const uint32_t VOICE_WINDOW_MS = 5 * 60 * 1000UL;
static uint32_t s_windowUntil = 0;
void voiceWake() { s_windowUntil = millis() + VOICE_WINDOW_MS; }
void voiceNapNow() { s_windowUntil = millis(); }   // bench: `listen nap`

// ---- mic reader while the recogniser naps ----
// The music bob lives on the mic blocks (micRead -> musicFeed), and the recogniser's feed task is
// normally the only reader. While it's off, this task reads instead. micRead isn't reentrant:
// start() raises s_srStarting and waits for the task's current block before ESP-SR takes over.
static volatile bool s_srStarting = false, s_micTaskBusy = false;
static void micTask(void*) {
  static int16_t buf[512];
  for (;;) {
    if (s_running || s_srStarting || !g_set.bobOn || !micOk()) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
    s_micTaskBusy = true;
    if (s_running || s_srStarting) { s_micTaskBusy = false; continue; }   // lost the race: hand over
    micRead(buf, 512);                                                    // ~32 ms, blocks on the DMA
    s_micTaskBusy = false;
  }
}

static bool start() {
  if (s_running) return true;
  if (!micOk() || !s_modelsOk) return false;
  s_srStarting = true;
  for (int i = 0; i < 40 && s_micTaskBusy; i++) delay(5);   // let the music reader finish its block
  ESP_SR.onEvent(onSr);
  s_running = ESP_SR.begin(s_stream, kCmds, N_CMDS, SR_CHANNELS_MONO, SR_MODE_WAKEWORD);
  s_srStarting = false;
  Serial.printf("voice: %s\n", s_running ? "listening for \"Hi ESP\"" : "ESP-SR start failed");
  return s_running;
}

// srmodels.bin starts with a little-endian model count, then each model's name (32 bytes).
// A new board's partition holds erased flash or leftover factory data, and ESP-SR crashes
// (LoadProhibited after "Please select wake words!") if it's started on that.
static bool modelsLookValid(const esp_partition_t* p) {
  uint8_t h[36];
  if (esp_partition_read(p, 0, h, sizeof(h)) != ESP_OK) return false;
  uint32_t n = h[0] | (h[1] << 8) | (h[2] << 16) | ((uint32_t)h[3] << 24);
  if (n < 1 || n > 64) return false;
  int len = 0;
  while (len < 32 && h[4 + len]) {
    if (h[4 + len] < 0x20 || h[4 + len] > 0x7E) return false;
    len++;
  }
  return len >= 2;
}

bool voiceInit() {
  const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
  if (!p) { Serial.println("voice: no 'model' partition (see README: Voice commands)"); return false; }
  s_modelsOk = modelsLookValid(p);
  if (!s_modelsOk) { Serial.println("voice: 'model' partition is empty - flash srmodels.bin (README: Voice commands)"); return false; }
  voiceWake();
  if (!g_set.voiceOn) return false;
  return start();
}

static void startMicTask() {
  static bool s_started = false;
  if (s_started || !micOk()) return;
  s_started = true;
  xTaskCreatePinnedToCore(micTask, "micbob", 3072, nullptr, 1, nullptr, 1);
}

void voiceSetEnabled(bool on) {
  if (on) start();
  else if (s_running) { ESP_SR.end(); s_running = false; brainVoice(Expr::Idle); Serial.println("voice: off"); }
}

bool voiceRunning() { return s_running; }

void voiceSetThreshold(float t) {
  s_thresh = t;
}

void voiceListenNow() {
  if (!s_running && !start()) return;
  ESP_SR.setMode(SR_MODE_COMMAND);
  s_event = 0;
}

void voiceStatus(char* b, size_t n) {
  snprintf(b, n, "voice: %s, models %s, mic %s, setting %s, min sure %.2f", s_running ? "running" : "stopped",
           s_modelsOk ? "found" : "MISSING", micOk() ? "ok" : "FAILED", g_set.voiceOn ? "on" : "off", s_thresh);
}

// ------------------------------------------------------------- actions --
static bool stopped() { return g_motion.stoppedSecs > 2.0f || !g_motion.ok; }

static void notNow() {                      // e.g. drag race while moving
  brainEvent(Expr::MenuLocked, 1500);
  audioChirp(400, 300, 140);
}

static void show(const char* label, const char* big, const char* sub, uint32_t ms = 3000) {
  brainSetAnswer(label, big, sub);
  brainEvent(Expr::Answer, ms);
}

static void showVolume() {
  brainSetVolume(g_set.volume);
  brainEvent(Expr::Volume, 2000);
}

static void act(int cmd) {
  char big[16], sub[28];
  switch (cmd) {
    case VC_DRAG_RACE:
      if (!stopped()) { notNow(); return; }
      launchArm();                                   // plays "Los geht's!" itself
      return;
    case VC_MENU:
      if (!stopped()) { notNow(); return; }
      menuOpen();
      break;
    case VC_BRIGHTER: case VC_DIMMER: {
      int* bri = &g_set.dayBri;   // one brightness (Nov 22 2026)
      *bri = constrain(*bri + (cmd == VC_BRIGHTER ? 15 : -15), 5, 100);
      settingsSave();
      snprintf(big, sizeof(big), "%d%%", *bri);
      show("BRIGHTNESS", big, "", 2000);
      break;
    }
    case VC_LOUDER: case VC_QUIETER:
      g_set.volume = constrain(g_set.volume + (cmd == VC_LOUDER ? 15 : -15), 0, 100);
      if (g_set.volume) s_lastVol = g_set.volume;
      settingsSave();
      showVolume();
      break;
    case VC_MUTE:
      if (g_set.volume) s_lastVol = g_set.volume;
      g_set.volume = 0;
      settingsSave();
      showVolume();
      return;                                        // no chirp, obviously
    default:
      if (cmd >= VC_BRIGHT && cmd < VC_BRIGHT + (int)(sizeof(kBriLevels) / sizeof(int))) {
        int* bri = &g_set.dayBri;   // "brightness fifty" etc.
        *bri = kBriLevels[cmd - VC_BRIGHT];
        settingsSave();
        snprintf(big, sizeof(big), "%d%%", *bri);
        show("BRIGHTNESS", big, "", 2000);
        break;
      }
      if (cmd >= VC_VOLUME && cmd < VC_VOLUME + (int)(sizeof(kVolLevels) / sizeof(int))) {
        g_set.volume = kVolLevels[cmd - VC_VOLUME];   // "volume thirty" etc.
        s_lastVol = g_set.volume;
        settingsSave();
        showVolume();
        break;
      }
      return;
    case VC_UNMUTE:
      g_set.volume = s_lastVol > 0 ? s_lastVol : 60;
      settingsSave();
      showVolume();
      break;
    case VC_BEST: {
      float b = launchBest(), rt = launchBestRt();
      if (b > 0) snprintf(big, sizeof(big), "%.1f S", b); else snprintf(big, sizeof(big), "--");
      if (rt > 0) snprintf(sub, sizeof(sub), "R/T .%03d", (int)(rt * 1000 + 0.5f)); else snprintf(sub, sizeof(sub), "NO R/T YET");
      show("BEST 0-60", big, sub, 4000);
      break;
    }
    case VC_SUMMARY:
      if (!tripShowSummary()) show("TRIP", "--", "NO DRIVE YET", 2500);
      break;
    case VC_SHOW:
      if (!stopped()) { notNow(); return; }
      extrasShow(true);
      break;
    case VC_SHOW_OFF:
      extrasShow(false);
      break;
    case VC_CALIBRATE:                               // like the BOOT hold, after a short delay
      // Not stopped(): a stale mount reads tilt as constant g, so he always looks "driving" exactly
      // when he needs this. Calibration restarts itself while shaken; only a real speed blocks it.
      if (g_speed.at && millis() - g_speed.at < 2000 && g_speed.mph > 2) { notNow(); return; }
      s_calAt = millis() + 1500;
      break;
    case VC_LEARN_ROAD:                              // while cruising: hands-free, no stopped() check
      motionStartRoadLearn();                        // (stoppedSecs counts up on a straight road)
      break;
    case VC_LEARN_ENGINE:                            // = menu ENGINE LEARN: parked, engine idling
      if (g_speed.at && millis() - g_speed.at < 2000 && g_speed.mph > 2) { notNow(); return; }
      motionStartEngineLearn();
      break;
    case VC_SLEEP:
      audioChirp(900, 500, 200);
      requestSleep();
      return;
    case VC_TIME: {
      time_t utc = rtcNowUtc();
      if (!rtcValid() || !utc) { show("TIME", "--:--", "CLOCK NOT SET"); break; }
      time_t loc = utc + tzOffsetMin() * 60;
      struct tm t;
      gmtime_r(&loc, &t);
      int h = t.tm_hour % 12; if (!h) h = 12;
      snprintf(big, sizeof(big), "%d:%02d", h, t.tm_min);
      show("TIME", big, t.tm_hour < 12 ? "AM" : "PM", 4000);
      break;
    }
  }
  audioChirp(1200, 1700, 90);                        // "got it"
}

void voiceUpdate(uint32_t now) {
  startMicTask();
  // follow the VOICE setting (menu, tuning page, cfg) and the listening window. Acts only when
  // "want" changes, so a recogniser that fails to start isn't retried every loop. Wi-Fi tuning
  // owns voice while it runs (it pauses it); pwrtest drives voiceOn directly (window ignored).
  static bool s_want = g_set.voiceOn;
  bool window = (int32_t)(s_windowUntil - now) > 0 || pwrtestActive();
  bool want = g_set.voiceOn && (window || s_listenSince);   // never nap in the middle of a command
  if (want != s_want && !wifiTuneActive()) {
    s_want = want;
    if (want) start();
    else if (s_running) {
      ESP_SR.end(); s_running = false; brainVoice(Expr::Idle);
      Serial.println(g_set.voiceOn ? "voice: napping (tap him to listen again)" : "voice: off");
    }
  }
  if (s_calAt && (int32_t)(now - s_calAt) >= 0) { s_calAt = 0; motionStartCalibration(); }
  if (!s_running) return;
  int ev = s_event;
  if (ev < 0) {
    if (s_listenSince) brainVoice(Expr::Listening);   // keep the face up while waiting
    return;
  }
  s_event = -1;
  voiceWake();                                        // any use keeps him listening
  if (ev == 0) {                                      // wake word
    Serial.println("voice: wake word");
    s_listenSince = now;
    brainVoice(Expr::Listening);
    audioChirp(1000, 1400, 70);
  } else if (ev == 1) {
    int cmd = s_cmd, ph = s_phrase;
    Serial.printf("voice: heard \"%s\" (cmd %d, sure %.2f", ph >= 0 && ph < N_CMDS ? kCmds[ph].str : "?", cmd,
                  sr_last_num ? sr_last_prob[0] : 0.0f);
    for (int i = 1; i < sr_last_num; i++) Serial.printf("; next cmd %d %.2f", sr_last_cmd[i], sr_last_prob[i]);
    Serial.println(")");
    s_listenSince = 0;
    brainVoice(Expr::Idle);
    if (sr_last_num && sr_last_prob[0] < s_thresh) ev = 2;   // not sure enough: treat as a miss
    else { s_misses = 0; act(cmd); }
  }
  if (ev == 2) {                                      // woke but no command understood (or not sure)
    if (s_listenSince) Serial.println("voice: timeout (no command)");
    else Serial.println("voice: not sure enough, ignored");
    s_listenSince = 0;
    brainVoice(Expr::Idle);
    if (++s_misses >= 2) {                            // second miss: show some examples
      s_misses = 0;
      show("TRY SAYING", "VOLUME LOW", "BRIGHTNESS HIGH / WHAT TIME IS IT", 4500);
    } else {
      brainEvent(Expr::Confused, 1600);
      audioChirp(700, 500, 120);
    }
  }
}
