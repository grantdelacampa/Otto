#include "drivelog.h"
#include "config.h"
#include "motion.h"
#include "brain.h"
#include "music.h"
#include "power.h"
#include "board.h"
#include "settings.h"
#include <LittleFS.h>

static const char* LOG = "/drive.csv";
static const char* LOG_OLD = "/drive.old";
static const size_t ROLL = 192 * 1024;
static String s_buf;
static uint32_t s_lastLine = 0, s_lastFlush = 0;
static bool s_header = false;

static void flushNow() {
  if (!s_buf.length()) return;
  File f = LittleFS.open(LOG, FILE_APPEND);
  if (f && f.size() > ROLL) {
    f.close();
    LittleFS.remove(LOG_OLD);
    LittleFS.rename(LOG, LOG_OLD);
    f = LittleFS.open(LOG, FILE_APPEND);
  }
  if (f) { f.print(s_buf); f.close(); }
  s_buf = "";
}

void drivelogFlush() { flushNow(); }

void drivelogEvent(const char* what) {
  if (!g_set.driveLogOn) return;
  char b[48];
  snprintf(b, sizeof(b), "# %lu %s\n", (unsigned long)(millis() / 1000), what);
  s_buf += b;
}

void drivelogUpdate(uint32_t now) {
  if (!g_motion.ok) return;
  if (!g_set.driveLogOn) { if (s_buf.length()) flushNow(); return; }   // DRIVE LOG off (menu)
  if (now - s_lastLine >= 2000) {
    s_lastLine = now;
    if (g_motion.stillSecs < 30) {           // a parked / desk Otto writes nothing
      if (!s_header) {
        s_header = true;
        char h[96];
        snprintf(h, sizeof(h), "# wake %lu (rtc), %d mV, tune %d\n", (unsigned long)rtcNowUtc(),
                 (int)(powerVolts() * 1000), TUNE_REV);   // TUNE_REV: docs/tuning/TUNING.md
        s_buf += h;
      }
      float lvl, floor, beat, l2, b2, bpm;
      musicDebug(lvl, floor, beat);
      musicSample(l2, b2, bpm);
      // t, vib (mg x10), road (mg x10), gyro (dps x10), stopped s, quiet s, engine, face, music
      // level dB, background dB, beat x100, bobbing; tune 2+: tempo estimate BPM, steady x100
      char b[140];
      snprintf(b, sizeof(b), "%lu,%d,%d,%d,%d,%d,%d,%s,%d,%d,%d,%d,%d,%d\n", (unsigned long)(now / 1000),
               (int)(g_motion.vib * 10000), (int)(g_motion.road * 10000), (int)(g_motion.gyroLP * 10),
               (int)g_motion.stoppedSecs, (int)g_motion.quietSecs, g_motion.engineOn,
               exprName(brainCurrent()), (int)lvl, (int)floor, (int)(beat * 100), musicHeard(),
               (int)bpm, (int)(musicSteady() * 100));
      s_buf += b;
    }
  }
  if (now - s_lastFlush >= 30000) { s_lastFlush = now; flushNow(); }
}

void drivelogDump(uint16_t src, void (*reply)(uint16_t, const char*)) {
  flushNow();
  reply(src, "# t,vib(0.1mg),road(0.1mg),gyro(0.1dps),stopped,quiet,eng,face,lvl,bg,beat,bob,bpm,steady");
  int n = 0;
  for (const char* path : {LOG_OLD, LOG}) {
    File f = LittleFS.open(path, FILE_READ);
    if (!f) continue;
    while (f.available()) {
      String s = f.readStringUntil('\n');
      s.trim();
      if (s.length()) { reply(src, s.c_str()); n++; }
    }
    f.close();
  }
  if (!n) reply(src, "drivelog: empty");
}

void drivelogClear() { s_buf = ""; LittleFS.remove(LOG); LittleFS.remove(LOG_OLD); }
