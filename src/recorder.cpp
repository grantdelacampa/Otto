#include "recorder.h"
#include <LittleFS.h>
#include <Preferences.h>

static const size_t CAP = 7200;  // 2 h at 1 Hz, 72 KB in PSRAM
static LogRec* s_buf = nullptr;
static size_t s_head = 0, s_count = 0;
static Peaks s_cur, s_last;
static bool s_curHasData = false;

static uint32_t s_secStart = 0;
static float s_mb = 0, s_ma = 0, s_ml = 0, s_my = 0, s_mu = 0;

static uint16_t enc(float v, float scale) {
  float x = v * scale;
  if (x < 0) x = 0;
  if (x > 65535) x = 65535;
  return (uint16_t)x;
}

static void push(const LogRec& r) {
  if (!s_buf) return;
  s_buf[s_head] = r;
  s_head = (s_head + 1) % CAP;
  if (s_count < CAP) s_count++;
}

void recorderInit() {
  s_buf = (LogRec*)ps_malloc(CAP * sizeof(LogRec));
  if (!s_buf) return;
  File f = LittleFS.open("/drive.bin", "r");
  if (f) {
    uint32_t n = 0;
    if (f.read((uint8_t*)&n, 4) == 4) {
      if (n > CAP) n = CAP;
      for (uint32_t i = 0; i < n; i++) {
        LogRec r;
        if (f.read((uint8_t*)&r, sizeof(r)) != sizeof(r)) break;
        push(r);
      }
    }
    f.close();
  }
  Preferences p;
  p.begin("otto", true);
  if (p.getBytesLength("peaks") == sizeof(Peaks)) p.getBytes("peaks", &s_last, sizeof(Peaks));
  p.end();
}

void recorderSample(uint32_t now, float brake, float accel, float lat, float yaw, float bump) {
  s_mb = fmaxf(s_mb, brake); s_ma = fmaxf(s_ma, accel); s_ml = fmaxf(s_ml, lat);
  s_my = fmaxf(s_my, yaw); s_mu = fmaxf(s_mu, bump);
  if (now - s_secStart < 1000) return;
  s_secStart = now;
  LogRec r = {enc(s_mb, 1000), enc(s_ma, 1000), enc(s_ml, 1000), enc(s_my, 10), enc(s_mu, 1000)};
  push(r);
  bool driving = s_ma > 0.08f || s_mb > 0.08f || s_ml > 0.08f;
  if (driving) {
    s_curHasData = true;
    s_cur.brake = fmaxf(s_cur.brake, s_mb); s_cur.accel = fmaxf(s_cur.accel, s_ma);
    s_cur.lat = fmaxf(s_cur.lat, s_ml); s_cur.yaw = fmaxf(s_cur.yaw, s_my); s_cur.bump = fmaxf(s_cur.bump, s_mu);
  }
  s_mb = s_ma = s_ml = s_my = s_mu = 0;
}

void recorderSave() {
  if (s_buf) {
    File f = LittleFS.open("/drive.bin", "w");
    if (f) {
      uint32_t n = s_count;
      f.write((uint8_t*)&n, 4);
      for (size_t i = 0; i < s_count; i++) {
        LogRec r = recorderGet(i);
        f.write((uint8_t*)&r, sizeof(r));
      }
      f.close();
    }
  }
  if (s_curHasData) {
    Preferences p;
    p.begin("otto", false);
    p.putBytes("peaks", &s_cur, sizeof(Peaks));
    p.end();
  }
}

void recorderResetPeaks() {
  s_cur = Peaks(); s_last = Peaks(); s_curHasData = false;
  Preferences p;
  p.begin("otto", false);
  p.remove("peaks");
  p.end();
}

Peaks recorderPeaks() { return s_curHasData ? s_cur : s_last; }
size_t recorderCount() { return s_count; }

LogRec recorderGet(size_t i) {
  size_t start = (s_head + CAP - s_count) % CAP;
  return s_buf[(start + i) % CAP];
}
