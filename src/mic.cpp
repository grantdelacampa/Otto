#include "mic.h"
#include "config.h"
#include "music.h"
#include <ESP_I2S.h>
#include <math.h>

// The ICS-43434 sends 24-bit samples in 32-bit I2S slots. Its L/R pin picks the slot; we read
// both and use whichever one carries the signal (found on the first read).
static I2SClass s_mic;
static bool s_ok = false;
static int s_chan = -1;          // 0 = left, 1 = right, -1 = not known yet
static const int SHIFT = 14;     // 32-bit slot -> 16-bit with ~+2 bits of gain (speech is quiet)

bool micInit() {
  s_mic.setPins(PIN_MIC_SCK, PIN_MIC_WS, -1, PIN_MIC_DIN);
  s_ok = s_mic.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
  Serial.printf("mic: %s\n", s_ok ? "ok" : "I2S init failed");
  return s_ok;
}

bool micOk() { return s_ok; }

size_t micRead(int16_t* out, size_t frames) {
  if (!s_ok) return 0;
  static int32_t raw[2 * 256];
  size_t done = 0;
  while (done < frames) {
    size_t want = min((size_t)256, frames - done);
    size_t got = s_mic.readBytes((char*)raw, want * 8) / 8;
    if (!got) break;
    if (s_chan < 0) {                            // pick the live channel
      int64_t e[2] = {0, 0};
      for (size_t i = 0; i < got; i++) { e[0] += llabs(raw[2 * i] >> 8); e[1] += llabs(raw[2 * i + 1] >> 8); }
      if (e[0] || e[1]) {
        s_chan = e[1] > e[0] ? 1 : 0;
        Serial.printf("mic: signal on the %s channel\n", s_chan ? "right" : "left");
      }
    }
    int c = s_chan < 0 ? 0 : s_chan;
    for (size_t i = 0; i < got; i++) {
      int32_t v = raw[2 * i + c] >> SHIFT;
      out[done + i] = (int16_t)constrain(v, -32768, 32767);
    }
    musicFeed(out + done, got);                  // music detection taps every block (cheap)
    done += got;
  }
  return done;
}

void micLevelTest(float secs, uint16_t src, void (*reply)(uint16_t, const char*)) {
  if (!s_ok) { reply(src, "mic: not available"); return; }
  if (secs <= 0 || secs > 60) secs = 10;
  static int16_t buf[1600];                      // 100 ms
  char line[96];
  float quietest = 0, loudest = -120;
  int blocks = (int)(secs * 10);
  micRead(buf, 1600);                            // discard the first block (start-up click)
  for (int b = 0; b < blocks; b++) {
    size_t n = micRead(buf, 1600);
    if (!n) { reply(src, "mic: no data"); return; }
    double mean = 0;
    for (size_t i = 0; i < n; i++) mean += buf[i];
    mean /= n;
    double sq = 0; int pk = 0;
    for (size_t i = 0; i < n; i++) {
      double v = buf[i] - mean;
      sq += v * v;
      pk = max(pk, (int)fabs(v));
    }
    float rmsDb = 20 * log10f(fmaxf(sqrt(sq / n), 1.0f) / 32768.0f);
    float pkDb = 20 * log10f(fmaxf(pk, 1) / 32768.0f);
    if (b == 0 || rmsDb < quietest) quietest = rmsDb;
    loudest = fmaxf(loudest, rmsDb);
    int bar = constrain((int)((rmsDb + 80) / 2), 0, 40);   // -80 dBFS .. 0
    int l = snprintf(line, sizeof(line), "mic %5.1f dB  pk %5.1f  ", rmsDb, pkDb);
    for (int i = 0; i < bar && l < (int)sizeof(line) - 1; i++) line[l++] = '#';
    line[l] = 0;
    reply(src, line);
  }
  snprintf(line, sizeof(line), "mic: quietest %.1f dB, loudest %.1f dB, difference %.1f dB",
           quietest, loudest, loudest - quietest);
  reply(src, line);
}
