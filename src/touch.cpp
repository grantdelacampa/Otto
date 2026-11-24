#include "touch.h"
#include "config.h"
#include "board.h"
#include "brain.h"
#include "audio.h"
#include "menu.h"
#include "motion.h"
#include "settings.h"
#include <Wire.h>

// ------------------------------------------------------------- SPD2010 ----
// Protocol follows Espressif's esp_lcd_touch_spd2010 driver (and Waveshare's 1.46B demo),
// done over Wire: the touch shares the main I2C bus with the IMU, RTC and IO expander.
// Registers are 16-bit (sent as two bytes); commands are 4-byte writes.
static const uint8_t CMD_POINT_MODE[4] = {0x50, 0x00, 0x00, 0x00};
static const uint8_t CMD_START[4]      = {0x46, 0x00, 0x00, 0x00};
static const uint8_t CMD_CPU_START[4]  = {0x04, 0x00, 0x01, 0x00};
static const uint8_t CMD_CLEAR_INT[4]  = {0x02, 0x00, 0x01, 0x00};
static const int MAX_PTS = 1;               // gestures use the first finger only

static bool s_ok = false;
static uint16_t s_fw = 0;
static volatile bool s_intFlag = false;

struct TouchSample { uint32_t t; int8_t n; int16_t x, y; };   // n: 1 = finger at x,y, 0 = lifted
static QueueHandle_t s_q = nullptr;
static volatile bool s_pause = false, s_qOverflow = false;
static void readerTask(void*);

static void IRAM_ATTR onTouchInt() { s_intFlag = true; }

static bool tpWrite(const uint8_t* d, size_t n) {
  Wire.beginTransmission(ADDR_TOUCH);
  Wire.write(d, n);
  bool ok = Wire.endTransmission() == 0;
  delayMicroseconds(200);
  return ok;
}

static bool tpRead(uint8_t r0, uint8_t r1, uint8_t* buf, size_t n) {
  const uint8_t reg[2] = {r0, r1};
  if (!tpWrite(reg, 2)) return false;
  if (Wire.requestFrom((int)ADDR_TOUCH, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

void touchResume() { s_pause = false; }

void touchPrepareSleep() {
  // Clear any pending report so INT is idle (high) when we decide whether touch may wake us.
  s_pause = true;
  delay(25);
  if (s_ok) tpWrite(CMD_CLEAR_INT, 4);
}

static void mapPoint(int16_t& x, int16_t& y) {
#if TOUCH_SWAP_XY
  int16_t t = x; x = y; y = t;
#endif
#if TOUCH_MIRROR_X
  x = LCD_W - 1 - x;
#endif
#if TOUCH_MIRROR_Y
  y = LCD_H - 1 - y;
#endif
  if (g_set.screenFlip) { x = LCD_W - 1 - x; y = LCD_H - 1 - y; }   // picture turned 180 deg (display.cpp)
}

// Returns number of points (0 = no finger), or -1 if nothing new was read.
static bool s_rawDbg = false;   // `touch raw`: print every status read
// `perf`: how long reads take and how far apart they are while a finger is down
uint32_t g_tpReads = 0, g_tpReadUs = 0, g_tpReadMaxUs = 0, g_tpGapMaxMs = 0;

static int readFrame(TouchPoint* pts) {
  uint8_t st[4];
  if (!tpRead(0x20, 0x00, st, 4)) return -1;
  if (s_rawDbg) Serial.printf("%6lu st %02X %02X len %u int %d\n", millis() % 1000000, st[0], st[1], st[2] | (st[3] << 8), digitalRead(PIN_TP_INT));
  bool ptExist = st[0] & 0x01, gesture = st[0] & 0x02, aux = st[0] & 0x08;
  bool inBios = (st[1] >> 6) & 1, inCpu = (st[1] >> 5) & 1, cpuRun = (st[1] >> 3) & 1;
  uint16_t len = st[2] | (st[3] << 8);

  if (inBios) { tpWrite(CMD_CLEAR_INT, 4); tpWrite(CMD_CPU_START, 4); return -1; }
  if (inCpu)  { tpWrite(CMD_POINT_MODE, 4); tpWrite(CMD_START, 4); tpWrite(CMD_CLEAR_INT, 4); return -1; }
  // No data. Status high byte 0x08 = running, nothing pending: no finger on the glass. That's how the
  // SPD2010 shows a lift (bench, Nov 14 2026: every touch ended 01 18 -> 00 08; holds of 3-5 s never
  // showed 00 08). 0x18 (INT pending, no data) is just an event to acknowledge.
  if (cpuRun && len == 0) {
    tpWrite(CMD_CLEAR_INT, 4);
    return (!ptExist && !gesture && !(st[1] & 0x10)) ? 0 : -1;
  }

  int n = -1;
  if (ptExist || gesture) {
    uint8_t buf[4 + 10 * 6];
    if (len >= 4 && len <= sizeof(buf) && tpRead(0x00, 0x03, buf, len)) {
      uint8_t id = buf[4];
      n = 0;
      if (id <= 0x0A && ptExist) {
        int cnt = (len - 4) / 6;
        for (int i = 0; i < cnt && n < MAX_PTS; i++) {
          const uint8_t* p = buf + 4 + i * 6;
          int16_t x = ((p[3] & 0xF0) << 4) | p[1];
          int16_t y = ((p[3] & 0x0F) << 8) | p[2];
          if (p[4] == 0) continue;               // zero weight = lifted
          if (x >= LCD_W || y >= LCD_H) continue; // junk
          mapPoint(x, y);
          pts[n].x = x; pts[n].y = y;
          n++;
        }
      }
    }
    // drain any remaining packets, then clear the interrupt
    for (int k = 0; k < 6; k++) {
      uint8_t hs[8];
      if (!tpRead(0xFC, 0x02, hs, 8)) break;
      uint8_t status = hs[5];
      uint16_t next = hs[2] | (hs[3] << 8);
      if (status == 0x82) { tpWrite(CMD_CLEAR_INT, 4); break; }
      if (status == 0x00 && next > 0 && next <= 64) {
        uint8_t junk[64];
        tpRead(0x00, 0x03, junk, next);
        continue;
      }
      break;
    }
  } else if (cpuRun && aux) {
    tpWrite(CMD_CLEAR_INT, 4);
  }
  return n;
}

bool touchInit() {
  boardExpanderWrite(EXIO_TOUCH_RST, false);
  delay(50);
  boardExpanderWrite(EXIO_TOUCH_RST, true);
  delay(200);
  pinMode(PIN_TP_INT, INPUT_PULLUP);
  uint8_t st[4];
  for (int i = 0; i < 5 && !s_ok; i++) {
    s_ok = tpRead(0x20, 0x00, st, 4);
    if (!s_ok) delay(20);
  }
  if (!s_ok) return false;
  // walk it out of BIOS/CPU mode into point mode
  TouchPoint p[MAX_PTS];
  for (int i = 0; i < 6; i++) { readFrame(p); delay(5); }
  uint8_t fw[18];
  if (tpRead(0x26, 0x00, fw, 18)) s_fw = fw[4] | (fw[5] << 8);
  attachInterrupt(digitalPinToInterrupt(PIN_TP_INT), onTouchInt, FALLING);
  s_q = xQueueCreate(48, sizeof(TouchSample));
  // core 1 next to the main loop, just above it: a read is ~1 ms every 10 ms
  xTaskCreatePinnedToCore(readerTask, "touch", 3072, nullptr, 2, nullptr, 1);
  Serial.printf("touch: SPD2010 ok (fw 0x%04X)\n", s_fw);
  return true;
}

// ------------------------------------------------------------ reader task --
// The main loop only comes round every ~100 ms (a full 412x412 frame), and the SPD2010 shows a lift
// only as an idle status on the next read (no INT pulse): between two quick taps that's often
// < 100 ms, so taps merged into one long touch. This task reads every 10 ms instead and queues
// time-stamped samples for the gesture code (touchUpdate, main loop).
static void readerTask(void*) {
  bool down = false;
  for (;;) {
    if (!s_pause && s_ok) {
      bool fresh = s_intFlag || digitalRead(PIN_TP_INT) == LOW;
      if (fresh || down) {                       // finger down: keep polling to catch the lift
        s_intFlag = false;
        TouchPoint pt[MAX_PTS];
        uint32_t t0 = micros();
        int n = readFrame(pt);
        uint32_t us = micros() - t0;
        g_tpReads++; g_tpReadUs += us; if (us > g_tpReadMaxUs) g_tpReadMaxUs = us;
        uint32_t now = millis();
        static uint32_t s_prev = 0;
        if (down && s_prev && now - s_prev > g_tpGapMaxMs) g_tpGapMaxMs = now - s_prev;
        s_prev = now;
        // A lift only counts after 3 idle reads in a row (~30 ms): right after touch-down (and now
        // and then while a finger slides) the chip shows a single idle read, which turned the start
        // of every swipe into a tap (bench, Nov 15 2026). Real lifts stay idle for 190+ ms.
        static int idleRun = 0;
        idleRun = (n == 0) ? idleRun + 1 : 0;
        if (n > 0 || (n == 0 && down && idleRun >= 3)) {
          TouchSample smp = {now, (int8_t)(n > 0 ? 1 : 0), pt[0].x, pt[0].y};
          if (n == 0) { smp.x = smp.y = 0; }
          if (xQueueSend(s_q, &smp, 0) != pdTRUE) s_qOverflow = true;
          down = n > 0;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

static void debugPause() {       // debug commands read the chip themselves: stop the task meanwhile
  if (!s_pause) { s_pause = true; delay(25); }
}

int touchRawRead(TouchPoint* pts) {   // poll regardless of INT, with the status printed
  debugPause();
  s_rawDbg = true;
  int n = readFrame(pts);
  s_rawDbg = false;
  return n;
}

int touchDebugRead(TouchPoint* pts, int maxPts) {
  debugPause();
  s_intFlag = false;              // poll every time: a lift doesn't pulse INT
  TouchPoint p[MAX_PTS];
  int n = readFrame(p);
  for (int i = 0; i < n && i < maxPts; i++) pts[i] = p[i];
  return n;
}

// ------------------------------------------------------------ gestures ----
static const float CX = LCD_W / 2.0f;
static const float PX_PER_DESIGN = LCD_W / 240.0f;
static const float CY = LCD_H / 2.0f;
static const float DRAG_MIN_R   = 25 * PX_PER_DESIGN;  // closer to the centre the angle is too jumpy
static const float FLICK_COAST  = 1.3f;   // let go: coast a little faster than the finger (feels like a flick)
static const float FLICK_DIZZY  = 600.0f;  // deg/s finger peak in the last 150 ms -> dizzy (a hard flick)
static const float STACK_DIZZY  = 1100.0f; // or flicks stacked in the same direction get it this fast

struct Gesture {
  bool active = false;
  uint32_t t0 = 0, last = 0;
  float x0 = 0, y0 = 0, x = 0, y = 0;
  int maxN = 0;
  bool holdFired = false;   // petting started
  bool tapHold = false;     // this touch began right after a tap
  bool menuFired = false;   // tap-then-hold completed
  // grab and spin: the rotor follows the finger around the centre
  bool hasAng = false, dragging = false;
  float ang = 0, w = 0;     // last finger angle (deg), smoothed angular speed (deg/s, + = clockwise)
  uint32_t angT = 0;
  float wPeak = 0;          // fastest recent finger speed (signed); the finger slows just before lifting
  uint32_t wPeakT = 0;
  float spin0 = 0;          // how fast it was already spinning when the finger came down
  // recent finger angle history (unwrapped), for the flick speed at release
  float unw = 0;            // total angle turned so far
  float hA[8]; uint32_t hT[8]; int hN = 0;
};
static Gesture g;
static uint32_t s_lastPoll = 0;
static uint32_t s_lastTapEnd = 0;
static uint32_t s_taps[6] = {0};
static int s_tapIdx = 0;
static Expr s_lastTapExpr = Expr::Idle;

static const uint32_t DOUBLE_GAP_MS = 400;   // max gap between the tap and the hold
static const uint32_t MENU_HOLD_MS  = 600;   // how long the second touch must stay down

bool touchActive() { return g.active; }

static bool carStopped() { return g_motion.stoppedSecs > 2.0f || !g_motion.ok; }

// -- reactions (menu closed) --
static void reactTap(uint32_t now) {
  // Timeslip showing: a tap closes it (it stays up 15 s otherwise)
  if (brainCurrent() == Expr::LaunchResult) {
    brainEventPrio(Expr::Happy, 900, 90);
    audioChirp(900, 1500, 80);
    return;
  }
  // Still grumpy: more poking keeps him grumpy (and grumbling) instead of cheering him up.
  // Grumpy and the tap faces share a priority, so a plain tap used to replace it right away.
  if (brainCurrent() == Expr::Grumpy) {
    brainEvent(Expr::Grumpy, 1800);              // same expression: extends it
    audioChirp(420, 300, 120);
    return;
  }
  s_taps[s_tapIdx++ % 6] = now;
  int recent = 0;
  for (uint32_t tt : s_taps) if (tt && now - tt < 2000) recent++;
  Serial.printf("touch: tap (%d in 2 s)\n", recent);
  if (recent >= 5) {
    memset(s_taps, 0, sizeof(s_taps));
    brainEvent(Expr::Grumpy, 2200);
    audioChirp(500, 300, 180);
    return;
  }
  static const Expr opts[] = {Expr::TapHappy, Expr::Wobble, Expr::Startled};
  Expr e;
  do { e = opts[esp_random() % 3]; } while (e == s_lastTapExpr);
  s_lastTapExpr = e;
  brainEvent(e, e == Expr::Wobble ? 1600 : 1300);
  if (e == Expr::Startled) audioChirp(1400, 1900, 70);
  else audioChirp(900, 1500, 110);
}

static void reactSwipe(float x0, float dy, float durMs) {
  bool right = x0 > CX;
  float v = fabsf(dy) / fmaxf(durMs, 20.0f);              // px/ms
  float mag = constrain(v * 900.0f, 250.0f, 2600.0f);    // deg/s
  float sign = (dy > 0 ? 1.0f : -1.0f) * (right ? 1.0f : -1.0f);  // down on right = clockwise
  brainSpinImpulse(sign * mag);
  brainEvent(Expr::Spinning, 1800);
  audioChirp(sign > 0 ? 700 : 1100, sign > 0 ? 1100 : 700, 90);
}

static void onMenuGesture() {
  if (!carStopped()) { brainEvent(Expr::MenuLocked, 1500); audioChirp(400, 300, 120); return; }
  menuOpen();
  audioChirp(800, 1600, 120);
}

static void onRelease(uint32_t now) {
  if (g.menuFired) { g = Gesture(); return; }
  if (g.maxN == 1 && !g.holdFired) {
    float dx = g.x - g.x0, dy = g.y - g.y0;
    float dist = sqrtf(dx * dx + dy * dy);
    float dur = (float)(g.last - g.t0);
    if (dist < 14 * PX_PER_DESIGN && dur < 450) {
      if (menuIsOpen()) menuTap(g.x0, g.y0);
      else if (motionEngineLearning()) motionEngineLearnTap();   // stop / skip a step
      else { reactTap(now); s_lastTapEnd = now; }
    } else if (dist < 14 * PX_PER_DESIGN) {
      Serial.printf("touch: not a tap - held %u ms\n", (unsigned)dur);   // too long for a tap, too short to pet
    } else if (g.dragging) {
      // let go of the rotor: it coasts on at the finger's speed. (The lift is only noticed
      // TOUCH_RELEASE_MS after the last report, so don't judge "paused" by the clock here: a finger
      // that stopped while still down already pulled g.w toward 0.)
      float w = g.w;
      if (g.last - g.wPeakT < 150 && fabsf(g.wPeak) > fabsf(w)) w = g.wPeak;
      // flick speed straight from the angle covered in the last ~120 ms (the start point = touch-down
      // angle if the whole flick was quicker than that)
      float wFlick = 0;
      if (g.hN > 0) {
        int last = (g.hN - 1) % 8, avail = g.hN < 8 ? g.hN : 8;
        float a0 = 0; uint32_t t0 = g.t0;
        for (int i = 1; i < avail; i++) {             // walk back until 120 ms before the last report
          int k = (g.hN - 1 - i) % 8;
          a0 = g.hA[k]; t0 = g.hT[k];
          if (g.hT[last] - t0 >= 120) break;
        }
        // the whole drag was shorter than that: measure from touch-down (angle 0 there)
        if (avail == g.hN && g.hT[last] - t0 < 120) { a0 = 0; t0 = g.t0; }
        uint32_t dt = g.hT[last] - t0;
        if (dt >= 20) wFlick = (g.hA[last] - a0) * 1000.0f / dt;
      }
      if (fabsf(wFlick) > fabsf(w)) w = wFlick;
      float coast = w * FLICK_COAST;
      // flicking again the same way while it still spins adds to it
      bool stacked = g.spin0 * w > 0 && fabsf(g.spin0) > 200;
      if (stacked) coast += g.spin0 * 0.6f;
      bool dizzy = fabsf(w) >= FLICK_DIZZY || fabsf(coast) >= STACK_DIZZY;
      Serial.printf("touch: let go at %.0f deg/s (now %.0f, peak %.0f, flick %.0f, %d reports, before %.0f)%s\n",
                    w, g.w, g.wPeak, wFlick, g.hN, g.spin0, dizzy ? " -> dizzy" : "");
      brainSpinRelease(coast, dizzy);
      if (fabsf(w) > 150) {
        brainEvent(Expr::Spinning, 1800);
        audioChirp(w > 0 ? 700 : 1100, w > 0 ? 1100 : 700, 90);
      }
    } else if (fabsf(dy) > 34 * PX_PER_DESIGN && fabsf(dy) > 1.5f * fabsf(dx)) {
      if (menuIsOpen()) menuSwipe(dy / PX_PER_DESIGN);
      else reactSwipe(g.x0, dy, dur);
    }
  }
  if (g.holdFired && !menuIsOpen()) brainEvent(Expr::Happy, 900);  // petting ends with a smile
  g = Gesture();
}

static void onSample(uint32_t now, int n, const TouchPoint* p);

void touchUpdate(uint32_t now) {
  if (!s_ok) return;
  s_pause = false;                 // a debug command that paused the reader has finished
  // Everything the reader task saw since last time, each with the time it was read
  TouchSample smp;
  while (xQueueReceive(s_q, &smp, 0) == pdTRUE) {
    TouchPoint pt = {smp.x, smp.y};
    onSample(smp.t, smp.n, &pt);
  }
  if (s_qOverflow) { s_qOverflow = false; Serial.println("touch: sample queue overflowed"); }
  // safety net: no report for a while with a finger down = lifted
  if (g.active && (int32_t)(now - g.last) > (int32_t)TOUCH_RELEASE_MS) onRelease(now);
}

static void onSample(uint32_t now, int n, const TouchPoint* p) {
  if (n == 0) {
    if (g.active) onRelease(now);
    return;
  }

  if (!g.active) {
    g.active = true;
    g.t0 = now;
    g.x0 = p[0].x; g.y0 = p[0].y;
    float rx = g.x0 - CX, ry = g.y0 - CY;
    g.hasAng = sqrtf(rx * rx + ry * ry) > DRAG_MIN_R;
    g.ang = atan2f(ry, rx) * RAD_TO_DEG; g.angT = now;
    g.spin0 = brainUserSpin();
    if (menuIsOpen()) menuTouched(now);
    else if (s_lastTapEnd && now - s_lastTapEnd < DOUBLE_GAP_MS) g.tapHold = true;
  }
  g.last = now;
  g.x = p[0].x; g.y = p[0].y;
  if (n > g.maxN) g.maxN = n;

  float dx = g.x - g.x0, dy = g.y - g.y0;
  bool still = sqrtf(dx * dx + dy * dy) < 14 * PX_PER_DESIGN;

  // tap, then tap-and-hold -> config menu
  if (g.tapHold && !menuIsOpen()) {
    if (!still) { g.tapHold = false; return; }   // moved: it's a swipe after all
    if (!g.menuFired) {
      float prog = (now - g.t0) / (float)MENU_HOLD_MS;
      // wait a moment so a plain double tap doesn't flash the ring
      if (carStopped() && now - g.t0 > 150) {
        brainSetRing(prog);
        brainEventPrio(Expr::MenuOpening, 150, 85);
      }
      if (prog >= 1.0f) { g.menuFired = true; onMenuGesture(); }
    }
    return;
  }

  // grab and spin: one finger moving on the face turns the rotor with it
  if (g.maxN == 1 && !menuIsOpen() && !g.holdFired && (g.dragging || !still)) {
    float rx = g.x - CX, ry = g.y - CY;
    float ang = atan2f(ry, rx) * RAD_TO_DEG;
    if (sqrtf(rx * rx + ry * ry) > DRAG_MIN_R) {
      // the same point again (a poll, not a new report): skip it unless the finger has really stopped
      bool same = fabsf(ang - g.ang) < 0.01f && now - g.angT < 120;
      if (same) return;
      if (g.hasAng && now > g.angT) {
        float da = ang - g.ang;
        if (da > 180) da -= 360;
        if (da < -180) da += 360;
        float w = da * 1000.0f / (now - g.angT);
        g.w = g.dragging ? g.w + (w - g.w) * 0.6f : w;   // first real step: take it as is
        g.unw += da;
        g.hA[g.hN % 8] = g.unw; g.hT[g.hN % 8] = now; g.hN++;
        if (fabsf(g.w) >= fabsf(g.wPeak) || now - g.wPeakT > 150) { g.wPeak = g.w; g.wPeakT = now; }
        g.dragging = true;
        brainSpinDrag(g.w);
        brainEvent(Expr::Spinning, 600);
      }
      g.ang = ang; g.angT = now; g.hasAng = true;
    } else g.hasAng = false;   // crossing the centre: pick the angle up again on the other side
    return;
  }

  // one finger held still -> petting
  if (g.maxN == 1 && !menuIsOpen()) {
    if (!g.holdFired && still && now - g.t0 > 800) {
      g.holdFired = true;
      audioChirp(500, 700, 160);
    }
    if (g.holdFired) brainEvent(Expr::Pet, 400);   // extended while held
  }
}
