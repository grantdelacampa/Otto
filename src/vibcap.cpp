#include "vibcap.h"
#include "motion.h"
#include "audio.h"
#include "settings.h"
#include <math.h>
#include <esp_heap_caps.h>

static const int   N = 2048;        // samples (power of two for the FFT)
static const float RATE = 900.0f;   // ~the IMU's accel-only ODR; sampling follows its data-ready flag

static float *s_ax, *s_ay, *s_az, *s_re, *s_im, *s_amp;

static bool alloc() {
  if (s_ax) return true;
  auto a = [](size_t n) { return (float*)heap_caps_malloc(n * sizeof(float), MALLOC_CAP_SPIRAM); };
  s_ax = a(N); s_ay = a(N); s_az = a(N); s_re = a(N); s_im = a(N); s_amp = a(N / 2 + 1);
  return s_ax && s_ay && s_az && s_re && s_im && s_amp;
}

// In-place iterative radix-2 FFT
static void fft(float* re, float* im, int n) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
  }
  for (int len = 2; len <= n; len <<= 1) {
    float ang = -2 * PI / len, wr = cosf(ang), wi = sinf(ang);
    for (int i = 0; i < n; i += len) {
      float cr = 1, ci = 0;
      for (int k = 0; k < len / 2; k++) {
        int a = i + k, b = a + len / 2;
        float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
        re[b] = re[a] - tr; im[b] = im[a] - ti;
        re[a] += tr; im[a] += ti;
        float ncr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = ncr;
      }
    }
  }
}

// Adds one axis' amplitude spectrum (g, Hann-corrected) squared into s_amp; returns the axis RMS (g)
static float addAxis(const float* x) {
  float mean = 0;
  for (int i = 0; i < N; i++) mean += x[i];
  mean /= N;
  float sq = 0, wsum = 0;
  for (int i = 0; i < N; i++) {
    float v = x[i] - mean, w = 0.5f - 0.5f * cosf(2 * PI * i / (N - 1));
    sq += v * v; wsum += w;
    s_re[i] = v * w; s_im[i] = 0;
  }
  fft(s_re, s_im, N);
  for (int k = 0; k <= N / 2; k++) {
    float a = 2 * sqrtf(s_re[k] * s_re[k] + s_im[k] * s_im[k]) / wsum;   // peak amplitude, g
    s_amp[k] += a * a;
  }
  return sqrtf(sq / N);
}

// RMS of the amplitude spectrum over [f0, f1] Hz
static float band(float f0, float f1, float binHz) {
  int k0 = max(1, (int)(f0 / binHz)), k1 = min(N / 2, (int)ceilf(f1 / binHz));
  float s = 0; int c = 0;
  for (int k = k0; k <= k1; k++) { s += s_amp[k] * s_amp[k]; c++; }
  return c ? sqrtf(s / c) : 0;
}

void vibcapRun(const char* mode, uint16_t src, bool serial, void (*reply)(uint16_t, const char*)) {
  char b[200];
  if (!alloc()) { reply(src, "vibcap: out of memory"); return; }
  float hz = 0;
  // "tone [Hz]": bench self-test - play a steady tone through Otto's speaker during the capture;
  // the board shakes at that frequency, so the spectrum should peak there.
  int savedVol = -1;
  if (!strncmp(mode, "tone", 4)) {
    float f = atof(mode + 4);
    if (f < 20 || f > 440) f = 200;
    savedVol = g_set.volume;
    g_set.volume = 100;                            // full volume for the test only (not saved)
    audioChirp(f, f, 3200);
    delay(150);
  }
  int n = motionVibCapture(s_ax, s_ay, s_az, N, RATE, hz);
  if (savedVol >= 0) { delay(400); g_set.volume = savedVol; }
  if (n < N || hz < 100) { reply(src, "vibcap: capture failed (IMU?)"); return; }

  for (int k = 0; k <= N / 2; k++) s_amp[k] = 0;
  float rx = addAxis(s_ax), ry = addAxis(s_ay), rz = addAxis(s_az);
  for (int k = 0; k <= N / 2; k++) s_amp[k] = sqrtf(s_amp[k]);       // combined over the 3 axes
  float binHz = hz / N, rms = sqrtf(rx * rx + ry * ry + rz * rz);

  snprintf(b, sizeof(b), "vibcap: %d samples @ %.0f Hz (%.2f Hz/bin), rms %.4f g", n, hz, binHz, rms);
  reply(src, b);

  // strongest local maxima above 3 Hz
  int top[5] = {-1, -1, -1, -1, -1};
  for (int k = (int)(3 / binHz) + 1; k < N / 2; k++) {
    if (s_amp[k] < s_amp[k - 1] || s_amp[k] < s_amp[k + 1]) continue;
    for (int t = 0; t < 5; t++) {
      if (top[t] < 0 || s_amp[k] > s_amp[top[t]]) {
        for (int u = 4; u > t; u--) top[u] = top[u - 1];
        top[t] = k;
        break;
      }
    }
  }
  int len = snprintf(b, sizeof(b), "vibcap: peaks");
  for (int t = 0; t < 5 && top[t] >= 0; t++)
    len += snprintf(b + len, sizeof(b) - len, " %.1fHz %.4fg%s", top[t] * binHz, s_amp[top[t]], t < 4 ? " |" : "");
  reply(src, b);

  // V8 at idle: firing = 4 x crank (RPM / 15); look for it at 500-1350 rpm = 33-90 Hz
  // (stops below a small 100 Hz peak the bare board shows on the bench).
  int fk = -1;
  for (int k = (int)(33 / binHz); k <= (int)(90 / binHz) && k < N / 2; k++)
    if (fk < 0 || s_amp[k] > s_amp[fk]) fk = k;
  float fire = fk * binHz, floor = band(3, 200, binHz);
  float half = fire / 8, halfAmp = band(half * 0.85f, half * 1.15f, binHz);
  snprintf(b, sizeof(b), "vibcap: firing %.1f Hz = %.0f rpm (V8, %.1fx floor); half-order %.1f Hz %.4f g (%.1fx floor)",
           fire, fire * 15, floor > 0 ? s_amp[fk] / floor : 0, half, halfAmp, floor > 0 ? halfAmp / floor : 0);
  reply(src, b);

  if (serial && !strcmp(mode, "csv")) {
    Serial.println("hz,g");
    for (int k = 0; k <= N / 2 && k * binHz <= hz / 2; k++) Serial.printf("%.2f,%.5f\n", k * binHz, s_amp[k]);
  } else if (serial && !strcmp(mode, "raw")) {
    Serial.println("ms,ax,ay,az");
    for (int i = 0; i < N; i++) Serial.printf("%.2f,%.5f,%.5f,%.5f\n", i * 1000.0f / hz, s_ax[i], s_ay[i], s_az[i]);
  }

  // Spectrum for the tuning page: bins up to 120 Hz in mg, 24 values per message
  if (!serial) {
    int kmax = min(N / 2, (int)(120 / binHz));
    snprintf(b, sizeof(b), "{\"vsh\":[%.4f,%d]}", binHz, kmax + 1);
    reply(src, b);
    for (int k0 = 0; k0 <= kmax; k0 += 24) {
      int l = snprintf(b, sizeof(b), "{\"vs\":[%d", k0);
      for (int k = k0; k < k0 + 24 && k <= kmax; k++) l += snprintf(b + l, sizeof(b) - l, ",%.2f", s_amp[k] * 1000);
      snprintf(b + l, sizeof(b) - l, "]}");
      reply(src, b);
      delay(20);                                   // don't flood BLE notifications
    }
  }
}
