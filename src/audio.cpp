#include "audio.h"
#include "config.h"
#include "settings.h"
#include <ESP_I2S.h>
#include <LittleFS.h>

static I2SClass s_i2s;
static QueueHandle_t s_q = nullptr;
static volatile bool s_busy = false;
static uint32_t s_rate = 0;

struct AudioReq {
  bool clip;
  char name[24];
  float f0, f1;
  uint16_t ms;
};

static float gain() {
  float v = g_set.volume / 100.0f;
  return v * v;
}

static void setRate(uint32_t rate) {
  if (rate == s_rate) return;
  s_i2s.configureTX(rate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  s_rate = rate;
}

static void playChirp(float f0, float f1, uint16_t ms) {
  setRate(16000);
  const int N = 256;
  int16_t buf[N * 2];
  int total = 16000 * ms / 1000;
  float phase = 0, g = gain() * 9000;
  for (int i = 0; i < total; i += N) {
    int n = min(N, total - i);
    for (int j = 0; j < n; j++) {
      float p = (float)(i + j) / total;
      float f = f0 + (f1 - f0) * p;
      float env = fminf(1.0f, fminf(p * 20, (1 - p) * 8));
      phase += 2 * PI * f / 16000.0f;
      int16_t s = (int16_t)(sinf(phase) * g * env);
      buf[2 * j] = s; buf[2 * j + 1] = s;
    }
    s_i2s.write((uint8_t*)buf, n * 4);
  }
}

static bool playWav(const char* name) {
  char path[40];
  snprintf(path, sizeof(path), "/clips/%s.wav", name);
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  char id[4];
  uint32_t sz;
  if (f.read((uint8_t*)id, 4) != 4 || memcmp(id, "RIFF", 4)) { f.close(); return false; }
  f.read((uint8_t*)&sz, 4);
  f.read((uint8_t*)id, 4);  // WAVE
  uint16_t ch = 1, bits = 16;
  uint32_t rate = 16000, dataLen = 0;
  while (f.available() >= 8) {
    f.read((uint8_t*)id, 4);
    f.read((uint8_t*)&sz, 4);
    if (!memcmp(id, "fmt ", 4)) {
      uint8_t fmt[16];
      f.read(fmt, 16);
      ch = fmt[2] | fmt[3] << 8;
      rate = fmt[4] | fmt[5] << 8 | fmt[6] << 16 | fmt[7] << 24;
      bits = fmt[14] | fmt[15] << 8;
      if (sz > 16) f.seek(f.position() + (sz - 16));
    } else if (!memcmp(id, "data", 4)) {
      dataLen = sz;
      break;
    } else {
      f.seek(f.position() + sz + (sz & 1));
    }
  }
  if (!dataLen || bits != 16 || ch < 1 || ch > 2) { f.close(); return false; }
  setRate(rate);
  const int N = 256;
  int16_t in[N * 2], out[N * 2];
  float g = gain();
  uint32_t left = dataLen;
  while (left > 0) {
    size_t want = min((uint32_t)(N * ch * 2), left);
    size_t got = f.read((uint8_t*)in, want);
    if (got == 0) break;
    left -= got;
    int frames = got / (2 * ch);
    for (int i = 0; i < frames; i++) {
      int16_t l = in[i * ch], r = ch == 2 ? in[i * ch + 1] : l;
      out[2 * i] = (int16_t)(l * g);
      out[2 * i + 1] = (int16_t)(r * g);
    }
    s_i2s.write((uint8_t*)out, frames * 4);
  }
  f.close();
  return true;
}

static void silence(int ms) {
  int16_t z[256 * 2] = {0};
  int frames = s_rate * ms / 1000;
  for (int i = 0; i < frames; i += 256) s_i2s.write((uint8_t*)z, min(256, frames - i) * 4);
}

static void audioTask(void*) {
  AudioReq r;
  for (;;) {
    if (xQueueReceive(s_q, &r, portMAX_DELAY) != pdTRUE) continue;
    if (g_set.volume <= 0) continue;
    s_busy = true;
    if (r.clip) {
      if (!playWav(r.name)) {  // no clip uploaded yet: friendly two-note chirp
        playChirp(880, 1320, 90);
        playChirp(1320, 1760, 120);
      }
    } else {
      playChirp(r.f0, r.f1, r.ms);
    }
    silence(30);
    s_busy = false;
  }
}

bool audioInit() {
  s_i2s.setPins(PIN_SPK_BCK, PIN_SPK_WS, PIN_SPK_DOUT);
  if (!s_i2s.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) return false;
  s_rate = 16000;
  s_q = xQueueCreate(4, sizeof(AudioReq));
  xTaskCreatePinnedToCore(audioTask, "audio", 6144, nullptr, 3, nullptr, 0);
  return true;
}

void audioPlayClip(const char* name) {
  if (!s_q) return;
  AudioReq r = {};
  r.clip = true;
  strncpy(r.name, name, sizeof(r.name) - 1);
  xQueueSend(s_q, &r, 0);
}

void audioChirp(float f0, float f1, uint16_t ms) {
  if (!s_q) return;
  AudioReq r = {};
  r.f0 = f0; r.f1 = f1; r.ms = ms;
  xQueueSend(s_q, &r, 0);
}

bool audioBusy() { return s_busy; }
