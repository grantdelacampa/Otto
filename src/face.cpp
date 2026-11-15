#include "face.h"
#include "display.h"
#include <math.h>

static const float S = LCD_W / 240.0f;  // design space -> panel pixels

// Current face transform, set per frame
static float g_fcx = 120, g_fcy = 120, g_fs = 1;
static float g_glow = 0.7f;

// face-space (rotor + eyes): follows the mini-face transform
// Where the piston strokes go: the canvas, or the piston cache (then shifted by its origin s_ox/s_oy)
static LGFX_Sprite* s_dst = &canvas;
static float s_ox = 0, s_oy = 0;
static inline float FX(float x) { return (g_fcx + (x - 120) * g_fs) * S - s_ox; }
static inline float FY(float y) { return (g_fcy + (y - 120) * g_fs) * S - s_oy; }
static inline float FW(float w) { return w * g_fs * S; }
// absolute design space (overlays, text)
static inline float AX(float x) { return x * S; }
static inline float AY(float y) { return y * S; }
static inline float AW(float w) { return w * S; }

static inline float lerpf(float a, float b, float k) { return a + (b - a) * k; }

void faceLerp(FaceState& c, const FaceState& t, float k) {
  c.spin = lerpf(c.spin, t.spin, k);
  c.tilt = lerpf(c.tilt, t.tilt, k);
  c.rscale = lerpf(c.rscale, t.rscale, k);
  c.rdx = lerpf(c.rdx, t.rdx, k);
  c.rdy = lerpf(c.rdy, t.rdy, k);
  c.rbright = lerpf(c.rbright, t.rbright, k);
  c.rest = lerpf(c.rest, t.rest, k);
  c.eyeR = lerpf(c.eyeR, t.eyeR, k);
  c.eyeDX = lerpf(c.eyeDX, t.eyeDX, k);
  c.eyeDY = lerpf(c.eyeDY, t.eyeDY, k);
  c.eyeBright = lerpf(c.eyeBright, t.eyeBright, k);
  c.fcx = lerpf(c.fcx, t.fcx, k);
  c.fcy = lerpf(c.fcy, t.fcy, k);
  c.fscale = lerpf(c.fscale, t.fscale, k);
  c.ring = t.ring;  // progress shouldn't lag
  c.drawFrac = t.drawFrac;
  c.bat = t.bat;
  c.vol = lerpf(c.vol, t.vol, k);
  c.eye = t.eye;
  c.brow = t.brow;
  c.mouth = t.mouth;
  c.gray = t.gray;
  c.ov = t.ov;
  c.dashed = t.dashed;
  c.captionColor = t.captionColor;
  c.batColor = t.batColor;
}

// ------------------------------------------------------------ primitives --
static void fLine(float x0, float y0, float x1, float y1, float w, uint16_t c) {
  s_dst->drawWideLine(FX(x0), FY(y0), FX(x1), FY(y1), FW(w) * 0.5f, c);
}
static void aLine(float x0, float y0, float x1, float y1, float w, uint16_t c) {
  canvas.drawWideLine(AX(x0), AY(y0), AX(x1), AY(y1), AW(w) * 0.5f, c);
}
static void fBez(float x0, float y0, float cx, float cy, float x1, float y1, float w, uint16_t c) {
  float px = x0, py = y0;
  for (int i = 1; i <= 10; i++) {
    float t = i / 10.0f, u = 1 - t;
    float x = u * u * x0 + 2 * u * t * cx + t * t * x1;
    float y = u * u * y0 + 2 * u * t * cy + t * t * y1;
    fLine(px, py, x, y, w, c);
    px = x; py = y;
  }
}
static void aBez(float x0, float y0, float cx, float cy, float x1, float y1, float w, uint16_t c) {
  float px = x0, py = y0;
  for (int i = 1; i <= 10; i++) {
    float t = i / 10.0f, u = 1 - t;
    float x = u * u * x0 + 2 * u * t * cx + t * t * x1;
    float y = u * u * y0 + 2 * u * t * cy + t * t * y1;
    aLine(px, py, x, y, w, c);
    px = x; py = y;
  }
}
static void fDot(float x, float y, float r, uint16_t c) { canvas.fillSmoothCircle(FX(x), FY(y), FW(r), c); }
static void aDot(float x, float y, float r, uint16_t c) { canvas.fillSmoothCircle(AX(x), AY(y), AW(r), c); }
static void fEllipse(float x, float y, float rx, float ry, uint16_t c) {
  int ryp = (int)FW(ry);
  if (ryp < 1) ryp = 1;
  canvas.fillEllipse(FX(x), FY(y), FW(rx), ryp, c);
}
static void aRing(float x, float y, float r, float w, float a0, float a1, uint16_t c) {
  canvas.fillArc(AX(x), AY(y), AW(r - w / 2), AW(r + w / 2), a0, a1, c);
}
// Smallest text height in design units (~3.1 mm on the 1.85" screen). Labels sized for the
// old 1.46" board (11-14 units, ~2.1-2.7 mm) were too small to read from the driver's seat.
static const float MIN_TEXT = 16;

// Set the font + size for a line of text: at least MIN_TEXT tall, then shrunk if needed to fit
// the round screen. The usable width is the circle's chord at the text's top/bottom edge
// (whichever is further from centre), minus a margin; alignment decides which side is the limit.
// Long lines like "Lange nicht gesehen!" otherwise run off the edges.
static void fitText(const char* s, float x, float y, const lgfx::IFont* font, float designH, float fontH, int align) {
  if (designH < MIN_TEXT) designH = MIN_TEXT;
  canvas.setFont(font);
  float sz = designH * S / fontH;
  canvas.setTextSize(sz, sz);
  float dy = fabsf(y - 120) + designH / 2;
  float r = 120 - 8;
  float h = dy < r ? sqrtf(r * r - dy * dy) : 0;           // half chord
  float maxW = align < 0 ? 120 + h - x : align > 0 ? x - (120 - h) : 2 * h;
  float w = canvas.textWidth(s) / S;
  if (maxW > 20 && w > maxW) {
    sz *= maxW / w;
    canvas.setTextSize(sz, sz);
  }
}

static void text(const char* s, float x, float y, const lgfx::IFont* font, float designH, float fontH,
                 uint16_t c) {
  fitText(s, x, y, font, designH, fontH, 0);
  canvas.setTextColor(c);
  canvas.setTextDatum(textdatum_t::middle_center);
  canvas.drawString(s, AX(x), AY(y));
}
static void zee(float x, float y, float w, uint16_t c) {
  aLine(x, y, x + w, y, 3, c);
  aLine(x + w, y, x, y + w, 3, c);
  aLine(x, y + w, x + w, y + w, 3, c);
}

static uint16_t pink(float b) { return mix565(C_BG, C_PINK, b); }
static uint16_t yellow(float b) { return mix565(C_BG, C_YELLOW, b); }
static float pulse(float t, float period, float lo = 0.3f) {
  return lo + (1 - lo) * (0.5f + 0.5f * cosf(t * 2 * PI / period));
}

// ---------------------------------------------------------------- otto ----
// Geometry is in the design's own units (docs/design/Otto.dc.html, viewBox 200x168), mapped into
// face space: SVG (100, 90) -> (120, 112), scale K. Two frames:
//   static frame (SX/SY): cylinder walls + crank case, follow only the whole-character offset
//   piston frame (PP):    the piston and its face, also scaled (rscale), leaned (tilt) and stroked
static const float K = 1.1f;
static const float CRANK_X = 100, CRANK_Y = 146;  // crankshaft centre
static const float THROW = 4;                     // crank throw: the stroke is 2 x THROW
static const float ROD = 44;                      // wrist pin (y 102) to big end (y 146)

static float s_dx = 0, s_dy = 0;   // whole-character offset (design units)
static float s_pdy = 0;            // piston stroke offset (SVG units, + = down)
static float s_hs = 1;             // piston scale
static float s_lc = 1, s_ls = 0;   // lean cos / sin
static float s_pinX = CRANK_X, s_pinY = CRANK_Y - THROW;

static inline float SX(float x) { return 120 + (x - 100) * K + s_dx; }
static inline float SY(float y) { return 112 + (y - 90) * K + s_dy; }
static void PP(float x, float y, float& ox, float& oy) {
  x = 100 + (x - 100) * s_hs;                     // scale about the piston centre (100, 66)
  y = 66 + (y - 66) * s_hs;
  float rx = x - 100, ry = y - 102;               // lean about the wrist pin (100, 102)
  ox = SX(100 + rx * s_lc - ry * s_ls);
  oy = SY(102 + rx * s_ls + ry * s_lc + s_pdy);
}
static void pLine(float x0, float y0, float x1, float y1, float w, uint16_t c) {
  float a, b, d, e;
  PP(x0, y0, a, b);
  PP(x1, y1, d, e);
  fLine(a, b, d, e, w * K * s_hs, c);
}
static void pBez(float x0, float y0, float cx, float cy, float x1, float y1, float w, uint16_t c) {
  float px, py;
  PP(x0, y0, px, py);
  for (int i = 1; i <= 8; i++) {
    float t = i / 8.0f, u = 1 - t, x, y;
    PP(u * u * x0 + 2 * u * t * cx + t * t * x1, u * u * y0 + 2 * u * t * cy + t * t * y1, x, y);
    fLine(px, py, x, y, w * K * s_hs, c);
    px = x; py = y;
  }
}
static void pTri(float x0, float y0, float x1, float y1, float x2, float y2, uint16_t c) {
  float a, b, d, e, g, h;
  PP(x0, y0, a, b);
  PP(x1, y1, d, e);
  PP(x2, y2, g, h);
  canvas.fillTriangle(FX(a), FY(b), FX(d), FY(e), FX(g), FY(h), c);
}
static void sRing(float x, float y, float r, float w, uint16_t c, float a0 = 0, float a1 = 360) {
  canvas.fillArc(FX(SX(x)), FY(SY(y)), FW((r - w / 2) * K), FW((r + w / 2) * K), a0, a1, c);
}

// Rounded piston outline (64,30)-(136,102), corner radius 10, clockwise from the top edge
static const int PN = 24;
static void pistonOutline(float* px, float* py) {
  static const float cx[4] = {126, 126, 74, 74}, cy[4] = {40, 92, 92, 40};
  int n = 0;
  for (int c = 0; c < 4; c++)
    for (int i = 0; i < 6; i++) {
      float a = (-90 + 90 * c + 18 * i) * DEG_TO_RAD;
      px[n] = cx[c] + 10 * cosf(a);
      py[n] = cy[c] + 10 * sinf(a);
      n++;
    }
}

// The stroke follows the crank: the big end runs round the crankshaft, the rod pulls the piston.
static void crankGeometry(float angleDeg, float rest) {
  float a = angleDeg * DEG_TO_RAD;
  float x = CRANK_X + THROW * sinf(a), y = CRANK_Y - THROW * cosf(a);
  s_pinX = x + (CRANK_X - x) * rest;              // asleep: settle at the bottom of the stroke
  s_pinY = y + (CRANK_Y + THROW - y) * rest;
  float dx = s_pinX - 100;
  s_pdy = s_pinY - sqrtf(ROD * ROD - dx * dx) - 102;
}

static void drawCylinder() {
  fLine(SX(56), SY(18), SX(56), SY(110), 3 * K, COL_HOUSING);
  fLine(SX(144), SY(18), SX(144), SY(110), 3 * K, COL_HOUSING);
  for (int i = 0; i < 14; i++) {                  // dashed crank case (design: dasharray 3 4)
    float a0 = i * 360.0f / 14;
    sRing(CRANK_X, CRANK_Y, 16, 2, COL_HOUSING, a0, a0 + 10.5f);
  }
}

static uint16_t s_body, s_bodyGlow, s_eye, s_eyeGlow;

// Glow strokes: a faint halo needs no anti-aliasing, and drawWideLine's smooth edges on the PSRAM
// canvas cost ~37 ms of an 80 ms frame (bench, Nov 15 2026). Plain triangles + round caps instead.
static void glowLine(float x0, float y0, float x1, float y1, float w, uint16_t c) {
  float ax = FX(x0), ay = FY(y0), bx = FX(x1), by = FY(y1), r = FW(w) * 0.5f;
  float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
  if (len > 0.5f) {
    float nx = -dy / len * r, ny = dx / len * r;
    s_dst->fillTriangle(ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny, c);
    s_dst->fillTriangle(ax + nx, ay + ny, bx - nx, by - ny, ax - nx, ay - ny, c);
  }
  s_dst->fillCircle(ax, ay, r, c);
}

// The piston body (fill, glow, outline, ring grooves) in its current pose. Filled with a colour one
// step off the background, so the cache below can use the exact background as "transparent" and
// still hide the rod's top end behind the piston.
static const uint16_t COL_BG_FILL = COL_BG ^ 0x0001;
static void drawPistonBody(const FaceState& f, bool glow, const float* qx, const float* qy) {
  float cx, cy;
  PP(100, 66, cx, cy);
  for (int i = 0; i < PN; i++)
    s_dst->fillTriangle(FX(cx), FY(cy), FX(qx[i]), FY(qy[i]), FX(qx[i + 1]), FY(qy[i + 1]), COL_BG_FILL);
  int segs = (int)(PN * f.drawFrac + 0.5f);
  float w = 5 * K * s_hs;
  if (glow)
    for (int i = 0; i < segs; i++) glowLine(qx[i], qy[i], qx[i + 1], qy[i + 1], w + 6, s_bodyGlow);
  for (int i = 0; i < segs; i++) {
    if (f.dashed && (i & 1)) continue;
    fLine(qx[i], qy[i], qx[i + 1], qy[i + 1], w, s_body);
  }
  if (f.drawFrac < 0.99f) return;                 // boot: outline only until it's fully drawn
  uint16_t ring = mix565(C_BG, s_body, 0.55f);    // piston ring grooves (design opacity .55)
  pLine(67, 44, 133, 44, 3, ring);
  pLine(67, 52, 133, 52, 3, ring);
}

// Piston cache: the body's smooth strokes cost ~30 ms a frame (bench, Nov 15 2026), but between
// pose changes the piston only moves (the stroke). So it's drawn once into a sprite and copied into
// place each frame; redrawn only when its shape or colours change (lean, size, brightness, glow).
// Brightness is applied while copying (idle breathes, so the colour changes every frame): the cache
// is drawn at full brightness in one hue, every pixel is "background -> hue by some amount", and the
// copy maps that amount through a 64-entry table built for this frame's brightness.
static uint16_t s_hue = COL_SILVER;                // body hue (silver / sleepy gray / lantern yellow)
static float s_bright = 1, s_glowK = 0;            // body brightness, glow strength (fraction of hue)
static LGFX_Sprite s_pc;
struct PcKey { int16_t lean, hs, fs, glowK; uint16_t hue; bool glow; };
static bool pcSame(const PcKey& a, const PcKey& b) {   // field by field (memcmp saw the padding byte)
  return a.lean == b.lean && a.hs == b.hs && a.fs == b.fs && a.glowK == b.glowK && a.hue == b.hue && a.glow == b.glow;
}
static PcKey s_pk;
static bool s_pkValid = false;
static float s_pcRefX = 0, s_pcRefY = 0;          // where the wrist pin sits inside the sprite
uint32_t g_pcRedraws = 0, g_pcWhy[6] = {0};                          // `perf`: cache redraws

static void drawPiston(const FaceState& f) {
  bool glow = g_glow > 0.02f && !f.dashed && !f.gray;
  // connecting rod: wrist (94..106, y 102) in the piston frame -> big end (97..103, 10 above the pin)
  float wlx, wly, wrx, wry;
  PP(94, 102, wlx, wly);
  PP(106, 102, wrx, wry);
  float blx = SX(s_pinX - 3), bly = SY(s_pinY - 10), brx = SX(s_pinX + 3);
  if (glow) {
    glowLine(wlx, wly, blx, bly, 9 * K, s_bodyGlow);
    glowLine(wrx, wry, brx, bly, 9 * K, s_bodyGlow);
    sRing(s_pinX, s_pinY, 10, 9, s_bodyGlow);
  }
  fLine(wlx, wly, blx, bly, 4 * K, s_body);
  fLine(wrx, wry, brx, bly, 4 * K, s_body);
  fLine(blx, bly, brx, bly, 4 * K, s_body);
  sRing(s_pinX, s_pinY, 10, 4, s_body);

  float ox[PN], oy[PN], qx[PN + 1], qy[PN + 1];
  pistonOutline(ox, oy);
  for (int i = 0; i < PN; i++) PP(ox[i], oy[i], qx[i], qy[i]);
  qx[PN] = qx[0]; qy[PN] = qy[0];

  // boot (outline being drawn) and the dashed update face change every frame: draw directly
  if (f.drawFrac < 0.99f || f.dashed) { drawPistonBody(f, glow, qx, qy); return; }

  float px, py;
  PP(100, 102, px, py);
  float refX = FX(px), refY = FY(py);
  // lean in half degrees (< 1 px at the piston's corners): idle tilt jitter redrew it constantly
  PcKey k = {(int16_t)lroundf(atan2f(s_ls, s_lc) * RAD_TO_DEG * 2), (int16_t)lroundf(s_hs * 400),
             (int16_t)lroundf(g_fs * 400), (int16_t)lroundf(s_glowK * 1000), s_hue, glow};
  if (s_pkValid) {
    extern uint32_t g_pcWhy[6];
    g_pcWhy[0] += k.lean != s_pk.lean; g_pcWhy[1] += k.hs != s_pk.hs; g_pcWhy[2] += k.fs != s_pk.fs;
    g_pcWhy[3] += k.hue != s_pk.hue; g_pcWhy[4] += k.glowK != s_pk.glowK; g_pcWhy[5] += k.glow != s_pk.glow;
  }
  if (!s_pkValid || !pcSame(k, s_pk)) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < PN; i++) {
      float sx = FX(qx[i]), sy = FY(qy[i]);
      x0 = fminf(x0, sx); x1 = fmaxf(x1, sx); y0 = fminf(y0, sy); y1 = fmaxf(y1, sy);
    }
    float m = FW(5 * K * s_hs + 6) * 0.5f + 3;     // glow / stroke half-width + a little
    int ix = (int)floorf(x0 - m), iy = (int)floorf(y0 - m);
    int w = (int)ceilf(x1 + m) - ix, h = (int)ceilf(y1 + m) - iy;
    if (s_pc.width() < w || s_pc.height() < h) {
      s_pc.deleteSprite();
      s_pc.setColorDepth(16);
      s_pc.setPsram(true);
      if (!s_pc.createSprite(w + 8, h + 8)) { s_pkValid = false; drawPistonBody(f, glow, qx, qy); return; }
    }
    s_pc.fillScreen(COL_BG);
    uint16_t body = s_body, bodyGlow = s_bodyGlow;  // full brightness in the cache
    s_body = s_hue; s_bodyGlow = mix565(C_BG, s_hue, s_glowK);
    s_dst = &s_pc; s_ox = ix; s_oy = iy;
    drawPistonBody(f, glow, qx, qy);
    s_dst = &canvas; s_ox = s_oy = 0;
    s_body = body; s_bodyGlow = bodyGlow;
    s_pcRefX = refX - ix; s_pcRefY = refY - iy;
    s_pk = k; s_pkValid = true;
    g_pcRedraws++;
  }
  // copy: background pixels stay transparent, the rest get this frame's brightness
  uint16_t lut[64];
  int gBg = (COL_BG >> 5) & 0x3F, gHue = (s_hue >> 5) & 0x3F;
  for (int g = 0; g < 64; g++) {
    float a = gHue != gBg ? (float)(g - gBg) / (gHue - gBg) : 0;
    uint16_t c = mix565(C_BG, s_hue, fminf(1, fmaxf(0, a)) * s_bright);
    lut[g] = (uint16_t)((c >> 8) | (c << 8));       // sprite buffers hold byte-swapped pixels
  }
  const uint16_t key = (uint16_t)((COL_BG >> 8) | (COL_BG << 8));
  int dx = (int)lroundf(refX - s_pcRefX), dy = (int)lroundf(refY - s_pcRefY);
  int sw = s_pc.width(), sh = s_pc.height();
  const uint16_t* src = (const uint16_t*)s_pc.getBuffer();
  uint16_t* dst = (uint16_t*)canvas.getBuffer();
  int x0 = max(0, dx), x1 = min(LCD_W, dx + sw), y0 = max(0, dy), y1 = min(LCD_H, dy + sh);
  for (int y = y0; y < y1; y++) {
    const uint16_t* sp = src + (y - dy) * sw + (x0 - dx);
    uint16_t* dp = dst + y * LCD_W + x0;
    for (int x = x0; x < x1; x++, sp++, dp++) {
      uint16_t v = *sp;
      if (v == key) continue;
      uint16_t c = (uint16_t)((v >> 8) | (v << 8));
      *dp = lut[(c >> 5) & 0x3F];
    }
  }
}

static void drawMouth(Mouth m, float t, bool yawn) {
  uint16_t c = s_body;
  if (yawn) {                                     // OV_YAWN: slow O
    float open = 0.25f + 0.75f * fmaxf(0, sinf(t * 2 * PI / 4.0f));
    pBez(95, 91, 100, 91 - 6 * open, 105, 91, 3, c);
    pBez(95, 91, 100, 91 + 8 * open, 105, 91, 3, c);
    return;
  }
  switch (m) {
    case Mouth::Flat:    pLine(94, 90, 106, 90, 3, c); break;
    case Mouth::Small:   pLine(96, 90, 104, 90, 3, c); break;
    case Mouth::Smile:   pBez(92, 88, 100, 95, 108, 88, 3, c); break;
    case Mouth::Pleased: pBez(93, 89, 100, 93, 107, 89, 3, c); break;
    case Mouth::Frown:   pBez(93, 91, 100, 88, 107, 91, 3, c); break;
    case Mouth::Grit:    pLine(93, 91, 107, 91, 3, c); break;
    case Mouth::O:
      pBez(96, 90, 100, 85, 104, 90, 3, c);
      pBez(96, 90, 100, 95, 104, 90, 3, c);
      break;
    case Mouth::Talk: {
      float amp = 1 + 3.5f * fabsf(sinf(t * 13.0f)) * (0.6f + 0.4f * sinf(t * 3.1f));
      pBez(94, 90, 100, 90 - amp, 106, 90, 3, c);
      pBez(94, 90, 100, 90 + amp, 106, 90, 3, c);
      break;
    }
    default: break;
  }
}

static void drawBrows(Brow b, float dy) {
  uint16_t c = s_eye;
  switch (b) {
    case Brow::Flat:       pLine(82, 66 + dy, 94, 66 + dy, 3, c); pLine(106, 66 + dy, 118, 66 + dy, 3, c); break;
    case Brow::Raised:     pLine(82, 63 + dy, 94, 63 + dy, 3, c); pLine(106, 63 + dy, 118, 63 + dy, 3, c); break;
    case Brow::Pleased:    pLine(82, 64 + dy, 94, 62 + dy, 3, c); pLine(106, 62 + dy, 118, 64 + dy, 3, c); break;
    case Brow::Determined: pLine(81, 63 + dy, 95, 69 + dy, 3.5f, c); pLine(119, 63 + dy, 105, 69 + dy, 3.5f, c); break;
    case Brow::Angry:      pLine(81, 61 + dy, 95, 70 + dy, 4, c); pLine(119, 61 + dy, 105, 70 + dy, 4, c); break;
    case Brow::Sad:        pLine(81, 69 + dy, 95, 64 + dy, 3, c); pLine(119, 69 + dy, 105, 64 + dy, 3, c); break;
    case Brow::Ahem:       pBez(81, 64 + dy, 88, 56 + dy, 95, 62 + dy, 3, c); pLine(106, 70 + dy, 118, 70 + dy, 3, c); break;
    case Brow::Uneven:     pBez(81, 63 + dy, 88, 57 + dy, 95, 62 + dy, 3, c); pLine(106, 67 + dy, 118, 67 + dy, 3, c); break;
    default: break;
  }
}

// --------------------------------------------------------------- eyes ----
// Rotor's eye shapes, scaled by s (Otto's eyes are smaller and closer together)
static void drawEye(Eye e, float x, float y, float r, float open, bool left, uint16_t c, uint16_t cg, float s) {
  switch (e) {
    case Eye::Dot:
      if (g_glow > 0.02f) fEllipse(x, y, r + 2.5f * s, (r + 2.5f * s) * open, cg);
      fEllipse(x, y, r, r * open, c);
      break;
    case Eye::Wide:
      if (g_glow > 0.02f) fDot(x, y, r + 2.5f * s, cg);
      fDot(x, y, r, c);
      fDot(x, y, r * 0.32f, COL_BG);
      break;
    case Eye::Happy:   fBez(x - 7 * s, y + 3 * s, x, y - 10 * s, x + 7 * s, y + 3 * s, 4.5f * s, c); break;
    case Eye::Content: fBez(x - 7 * s, y - 3 * s, x, y + 7 * s, x + 7 * s, y - 3 * s, 4.5f * s, c); break;
    case Eye::Closed:  fLine(x - 7 * s, y, x + 7 * s, y, 4.5f * s, c); break;
    case Eye::Cross:
      fLine(x - 6 * s, y - 6 * s, x + 6 * s, y + 6 * s, 4.5f * s, c);
      fLine(x + 6 * s, y - 6 * s, x - 6 * s, y + 6 * s, 4.5f * s, c);
      break;
    case Eye::Spiral:
      canvas.fillArc(FX(x), FY(y), FW(8 * s), FW(10.5f * s), 0, 360, c);
      canvas.fillArc(FX(x), FY(y), FW(2 * s), FW(4.5f * s), 0, 360, c);
      break;
    case Eye::Chevron: {
      float d = left ? 1 : -1;
      fLine(x - 7 * s * d, y - 8 * s, x + 5 * s * d, y, 5 * s, c);
      fLine(x + 5 * s * d, y, x - 7 * s * d, y + 8 * s, 5 * s, c);
      break;
    }
    case Eye::Half: fEllipse(x, y + r * 0.2f, r, r * 0.47f * open, c); break;
    case Eye::Wavy: {
      float px = x - 8 * s, py = y;
      for (int i = 1; i <= 8; i++) {
        float xx = x - 8 * s + 16.0f * s * i / 8, yy = y - 2.5f * s * sinf(i * PI / 4);
        fLine(px, py, xx, yy, 3.5f * s, c);
        px = xx; py = yy;
      }
      break;
    }
    case Eye::Uneven:
      if (left) fEllipse(x, y + 2 * s, r * 1.15f, r * 1.15f * open, c);
      else fEllipse(x, y - 2 * s, r * 0.65f, r * 0.65f * open, c);
      break;
    case Eye::Ahem:                                // design: left round, right half-lidded
      if (left) drawEye(Eye::Dot, x, y, r, open, left, c, cg, s);
      else fEllipse(x, y + 1 * s, r, r * 0.47f * open, c);
      break;
  }
}

// ----------------------------------------------------------- overlays ----
// Battery sits low on the right, clear of the crank case
static void drawBattery(const FaceState& f) {
  uint16_t c = f.batColor ? f.batColor : COL_YELLOW;
  const float bx = 150, by = 150;
  for (int i = 0; i < 3; i++)
    canvas.drawRect(AX(bx) + i, AY(by) + i, AW(40) - 2 * i, AW(20) - 2 * i, c);
  canvas.fillRect(AX(bx + 40), AY(by + 6), AW(4), AW(8), c);
  float w = 32 * constrain(f.bat, 0.0f, 1.0f);
  if (w > 1) canvas.fillRect(AX(bx + 4), AY(by + 4), AW(w), AW(12), c);
}

static void drawOverlays(const FaceState& f, const FaceText& tx, float t) {
  uint64_t ov = f.ov;   // 64-bit: OV_* bits go up to 48
  if (ov & OV_ZZ) { zee(172, 44, 13, COL_YELLOW); zee(190, 28, 9, COL_YELLOW); }
  if (ov & OV_SOUND) {
    uint16_t c = pink(pulse(t, 1.2f));
    aBez(212, 98, 222, 120, 212, 142, 4, c);
    aBez(28, 98, 18, 120, 28, 142, 4, c);
  }
  if (ov & OV_SPEAK) {
    uint16_t c = yellow(pulse(t, 1.0f));
    aBez(178, 66, 184, 80, 178, 94, 3, c);
    aBez(188, 60, 196, 80, 188, 100, 3, c);
  }
  if (ov & OV_THINKDOTS) {
    for (int i = 0; i < 3; i++) aDot(180 + 12 * i, 104, 4, yellow(pulse(t - i * 0.2f, 1.2f)));
  }
  if (ov & OV_DOTS3) {
    for (int i = 0; i < 3; i++) aDot(178 + 11 * i, 58, 3, COL_YELLOW);
  }
  if (ov & OV_NOTE) {
    canvas.fillEllipse(AX(176), AY(52), AW(8), AW(6), COL_YELLOW);
    aLine(183, 52, 183, 26, 3, COL_YELLOW);
    aLine(183, 26, 195, 32, 3, COL_YELLOW);
  }
  if (ov & OV_SPEEDTOP) {
    aLine(84, 22, 92, 34, 3, COL_YELLOW);
    aLine(120, 16, 120, 30, 3, COL_YELLOW);
    aLine(156, 22, 148, 34, 3, COL_YELLOW);
  }
  if (ov & OV_TRAIL) {
    aLine(12, 100, 36, 100, 3, COL_YELLOW);
    aLine(8, 120, 30, 120, 3, COL_YELLOW);
    aLine(12, 140, 36, 140, 3, COL_YELLOW);
  }
  if (ov & OV_SIDELINES) {                     // launch (design): ice-blue lines both sides
    uint16_t c = mix565(C_BG, C_ICE, 0.8f);
    static const float L[3][3] = {{22, 42, 70}, {18, 44, 88}, {22, 42, 106}};   // SVG x0, x1; design y
    for (int i = 0; i < 3; i++) {
      aLine(SX(L[i][0]) - s_dx, L[i][2], SX(L[i][1]) - s_dx, L[i][2], 3, c);
      aLine(SX(200 - L[i][1]) - s_dx, L[i][2], SX(200 - L[i][0]) - s_dx, L[i][2], 3, c);
    }
  }
  if (ov & OV_AHEM) text("AHEM.", 186, 56, &fonts::Font4, 20, 26, COL_ICE);
  if (ov & OV_TWINKLE) {                       // design: two 4-point stars, twinkling out of step
    static const float kX[2] = {186, 52}, kY[2] = {62, 98}, kR[2] = {9, 7.5f};
    for (int i = 0; i < 2; i++) {
      float p = 0.5f + 0.5f * sinf((t - i * 0.6f) * 2 * PI / 1.6f);
      float r = kR[i] * (0.6f + 0.4f * p), q = r * 0.25f, x = kX[i], y = kY[i];
      uint16_t c = yellow(0.2f + 0.8f * p);
      canvas.fillTriangle(AX(x), AY(y - r), AX(x - q), AY(y), AX(x + q), AY(y), c);
      canvas.fillTriangle(AX(x), AY(y + r), AX(x - q), AY(y), AX(x + q), AY(y), c);
      canvas.fillTriangle(AX(x - r), AY(y), AX(x), AY(y - q), AX(x), AY(y + q), c);
      canvas.fillTriangle(AX(x + r), AY(y), AX(x), AY(y - q), AX(x), AY(y + q), c);
    }
  }
  if (ov & OV_ARCS_R) { aBez(196, 92, 206, 120, 196, 148, 3, COL_YELLOW); aBez(212, 100, 220, 120, 212, 140, 3, COL_YELLOW); }
  if (ov & OV_ARCS_L) { aBez(44, 92, 34, 120, 44, 148, 3, COL_YELLOW); aBez(28, 100, 20, 120, 28, 140, 3, COL_YELLOW); }
  if (ov & OV_JOLT) {
    const float xs[] = {76, 88, 100, 112, 124, 136, 148, 160};
    for (int i = 0; i < 7; i++) aLine(xs[i], (i & 1) ? 202 : 212, xs[i + 1], (i & 1) ? 212 : 202, 3, COL_YELLOW);
  }
  if (ov & OV_ORBIT) {
    float base = t * 120.0f;
    for (int i = 0; i < 3; i++) {
      float a = (base + 120 * i - 90) * DEG_TO_RAD;
      aDot(120 + 90 * cosf(a), 120 + 90 * sinf(a), 5, COL_YELLOW);
    }
  }
  if (ov & OV_BADGE42) {                       // M119 4.2 V8, low on the right like the battery
    canvas.fillRect(AX(150), AY(146), AW(56), AW(28), COL_BG);
    for (int i = 0; i < 3; i++) canvas.drawRect(AX(150) + i, AY(146) + i, AW(56) - 2 * i, AW(28) - 2 * i, COL_YELLOW);
    text("4.2", 178, 161, &fonts::Font4, 18, 26, COL_YELLOW);
  }
  if (ov & OV_RING) {
    aRing(120, 120, 108, 6, 0, 360, COL_GRID);
    if (f.ring > 0.01f) aRing(120, 120, 108, 6, -90, -90 + 360 * f.ring, COL_ICE);
    text("HOLD STILL", 120, 152, &fonts::Font2, 14, 16, COL_YELLOW);
  }
  if (ov & OV_VOLUME) {
    aRing(120, 120, 106, 8, 135, 405, COL_GRID);
    aRing(120, 120, 106, 8, 135, 135 + 270 * f.vol / 100.0f, COL_ICE);
    char b[16];
    snprintf(b, sizeof(b), "VOL %d", (int)(f.vol + 0.5f));
    text(b, 120, 214, &fonts::Font4, 18, 26, COL_YELLOW);
  }
  if (ov & OV_QUESTION) text("?", 186, 62, &fonts::Font4, 36, 26, COL_YELLOW);
  if (ov & OV_BATTERY) drawBattery(f);
  if (ov & OV_THERMO) {                        // coolant thermometer, low on the right
    uint16_t c = f.batColor ? f.batColor : COL_ICE;
    const float x = 186, top = 112, bot = 160;
    aLine(x, top, x, bot, 9, COL_HOUSING);
    aDot(x, bot + 6, 8, COL_HOUSING);
    float lvl = bot - (bot - top) * constrain(f.bat, 0.0f, 1.0f);
    aLine(x, lvl, x, bot, 5, c);
    aDot(x, bot + 6, 5.5f, c);
    for (int i = 0; i < 4; i++) aLine(x + 7, top + 6 + 12 * i, x + 11, top + 6 + 12 * i, 2, COL_GRAY);
  }
  if (ov & OV_SHIFT) {                         // shift light: the edge ring flashes yellow / pink
    bool on = fmodf(t, 0.24f) < 0.14f;
    if (on) aRing(120, 120, 112, 8, 0, 360, fmodf(t, 0.48f) < 0.24f ? COL_YELLOW : COL_PINK);
    text("SHIFT", 120, 22, &fonts::Font4, 16, 26, on ? COL_YELLOW : COL_GRAY);
  }
  if (ov & OV_SWEAT) {
    aDot(184, 84, 7, COL_ICE);
    canvas.fillTriangle(AX(177.5f), AY(81), AX(190.5f), AY(81), AX(184), AY(64), COL_ICE);
  }
  if (ov & OV_CHECK) {
    aLine(88, 206, 96, 214, 4, COL_YELLOW);
    aLine(96, 214, 110, 198, 4, COL_YELLOW);
    text(tx.caption, 148, 207, &fonts::Font2, 16, 16, COL_ICE);
  }
  if (ov & OV_HEARTS) {
    float p = fmodf(t, 2.0f) / 2.0f;
    float y = 70 - 22 * p;                     // design: pink heart to the right of the cylinder
    uint16_t c = pink(p < 0.2f ? p / 0.2f : 1 - (p - 0.2f) / 0.8f);
    aDot(183, y, 5, c);
    aDot(193, y, 5, c);
    canvas.fillTriangle(AX(178.5f), AY(y + 2), AX(197.5f), AY(y + 2), AX(188), AY(y + 13), c);
  }
  if (ov & OV_SPARKLE) {
    uint16_t c = yellow(pulse(t, 0.8f, 0.4f));
    aLine(186, 59, 186, 79, 3, c); aLine(176, 69, 196, 69, 3, c);
    aLine(52, 64, 52, 78, 2, c);   aLine(45, 71, 59, 71, 2, c);
  }
  if (ov & OV_EXCLAIM) {
    canvas.fillRect(AX(185), AY(40), AW(7), AW(24), COL_YELLOW);
    aDot(188.5f, 72, 4, COL_YELLOW);
  }
  if (ov & OV_STEAM) {
    float p = fmodf(t, 2.0f) / 2.0f, dy = -10 * p;
    const uint16_t c = COL_GRAY;
    aBez(182, 74 + dy, 176, 66 + dy, 182, 58 + dy, 3, c);
    aBez(182, 58 + dy, 188, 50 + dy, 182, 42 + dy, 3, c);
    aBez(196, 80 + dy, 190, 72 + dy, 196, 64 + dy, 3, c);
  }
  if (ov & OV_LOCK) {
    for (int i = 0; i < 3; i++) canvas.drawRect(AX(176) + i, AY(56) + i, AW(20) - 2 * i, AW(16) - 2 * i, COL_YELLOW);
    aLine(180, 56, 180, 50, 3, COL_YELLOW);
    aBez(180, 50, 186, 40, 192, 50, 3, COL_YELLOW);
    aLine(192, 50, 192, 56, 3, COL_YELLOW);
  }
  if (ov & OV_PUFFS) {
    static const uint8_t SMOKE[3] = {154, 167, 182};
    for (int i = 0; i < 3; i++) {
      float p = fmodf(t / 0.9f + i / 3.0f, 1.0f);
      float r = 8 * (0.5f + 0.9f * p);
      uint16_t c = mix565(C_BG, SMOKE, 1 - p);
      aDot(64 - 26 * p, 196 + 10 * p, r, c);
      aDot(176 + 26 * p, 196 + 10 * p, r, c);
    }
  }
  if (ov & OV_LIGHTS) {
    float c = fmodf(t, 2.4f);
    const uint16_t off = rgb565(58, 52, 32);
    aDot(92, 40, 9, c > 0.72f ? COL_YELLOW : off);
    aDot(120, 40, 9, c > 1.32f ? COL_YELLOW : off);
    aDot(148, 40, 9, c > 1.92f ? COL_ICE : off);
  }
  if (ov & OV_TREE) {
    // Drag-strip tree on the right (docs/faces/21-drag-race.html), absolute design space
    canvas.fillSmoothRoundRect(AX(152), AY(40), AW(40), AW(144), AW(12), rgb565(17, 21, 27));
    for (int i = 0; i < 2; i++)
      canvas.drawRoundRect(AX(152) + i, AY(40) + i, AW(40) - 2 * i, AW(144) - 2 * i, AW(12) - i, COL_HOUSING);
    uint8_t b = tx.tree;
    auto bulb = [&](float x, float y, float r, bool lit, uint16_t on, uint16_t off) {
      if (lit && g_glow > 0.02f) aDot(x, y, r + 4, mix565(C_BG, on, 0.25f * g_glow));
      aDot(x, y, lit ? r + 1 : r, lit ? on : off);
    };
    const uint16_t dimY = rgb565(58, 52, 32), dimS = rgb565(47, 58, 71);
    bulb(164, 52, 3, b & TREE_PRESTAGE, COL_YELLOW, dimS);
    bulb(180, 52, 3, b & TREE_PRESTAGE, COL_YELLOW, dimS);
    bulb(164, 64, 3, b & TREE_STAGE, COL_YELLOW, dimS);
    bulb(180, 64, 3, b & TREE_STAGE, COL_YELLOW, dimS);
    bulb(172, 84, 8, b & TREE_A1, COL_YELLOW, dimY);
    bulb(172, 104, 8, b & TREE_A2, COL_YELLOW, dimY);
    bulb(172, 124, 8, b & TREE_A3, COL_YELLOW, dimY);
    bulb(172, 146, 8, b & TREE_GREEN, COL_ICE, rgb565(29, 58, 54));
    bool red = (b & TREE_RED) && fmodf(t, 0.5f) < 0.3f;     // flashes
    bulb(172, 168, 8, red, rgb565(255, 92, 122), rgb565(58, 31, 42));
    if (b & TREE_RED) {                                     // sweat drop on the wincing face
      canvas.fillTriangle(AX(114), AY(70), AX(108.8f), AY(80), AX(119.2f), AY(80), COL_ICE);
      aDot(114, 80.5f, 5.4f, COL_ICE);
    }
  }
  if (ov & OV_STREAKS) {
    float s = fmodf(t * 3, 1.0f) * 10;                    // streaks rush backwards
    aLine(18 - s, 102, 38 - s, 102, 3, COL_ICE);
    aLine(14 - s, 120, 40 - s, 120, 3, COL_ICE);
    aLine(18 - s, 138, 38 - s, 138, 3, COL_ICE);
    text("GO!", 120, 214, &fonts::Font4, 30, 26, COL_ICE);
  }
  if (ov & OV_SLIP) {
    text("TIMESLIP", 120, 58, &fonts::Font2, 16, 16, COL_YELLOW);
    for (float x = 62; x < 178; x += 11) { aLine(x, 72, fminf(x + 6, 178), 72, 2, COL_HOUSING); aLine(x, 192, fminf(x + 6, 178), 192, 2, COL_HOUSING); }
    static const char* kKeys[] = {"R/T", "0-30", "0-60", "PEAK"};
    for (int i = 0; i < 4; i++) {
      float y = 102 + 26 * i;
      faceText(kKeys[i], 60, y, 4, 18, COL_GRAY, Align::Left);
      faceText(tx.slip[i][0] ? tx.slip[i] : "--", 180, y, 4, 18, i == 2 ? COL_YELLOW : COL_ICE, Align::Right);
    }
  }
  if (ov & OV_RUMBLE) {
    const float ys[] = {104, 112, 120, 128, 136};
    for (int i = 0; i < 4; i++) {
      aLine((i & 1) ? 28 : 20, ys[i], (i & 1) ? 20 : 28, ys[i + 1], 3, COL_YELLOW);
      aLine((i & 1) ? 212 : 220, ys[i], (i & 1) ? 220 : 212, ys[i + 1], 3, COL_YELLOW);
    }
  }
  if (ov & (OV_UPDATE | OV_SPEEDRING | OV_OPENRING)) {
    uint16_t c = (ov & OV_SPEEDRING) ? COL_YELLOW : COL_ICE;
    aRing(120, 120, 108, 6, 0, 360, COL_GRID);
    if (f.ring > 0.005f) aRing(120, 120, 108, 6, -90, -90 + 360 * fminf(1.0f, f.ring), c);
  }
  if (ov & OV_UPDATE) {
    char b[8];
    snprintf(b, sizeof(b), "%d%%", (int)(f.ring * 100 + 0.5f));
    text("UPDATING", 120, 122, &fonts::Font2, 14, 16, COL_ICE);
    text(b, 120, 164, &fonts::Font4, 38, 26, COL_ICE);
    text("DON'T UNPLUG", 120, 200, &fonts::Font2, 15, 16, pink(pulse(t, 1.0f)));
  }
  if (ov & OV_CAPTION) text(tx.caption, 120, 216, &fonts::Font4, 16, 26, f.captionColor ? f.captionColor : COL_PINK);
  if (ov & OV_GREETING) {
    // German greeting (the Japan Gothic font covers Latin-1: umlauts and ß), English under it
    text(tx.greet, 120, 160, &fonts::lgfxJapanGothic_40, 30, 40, COL_SILVER);
    text(tx.gloss, 120, 194, &fonts::Font4, 17, 26, COL_PINK);
  }
  if (ov & OV_ANSWER) {
    text(tx.label, 120, 132, &fonts::Font4, 17, 26, COL_ICE);
    bool digits = true;
    for (const char* p = tx.big; *p; p++)
      if (!strchr("0123456789:.- ", *p)) digits = false;
    if (digits) text(tx.big, 120, 174, &fonts::Font7, 40, 48, COL_ICE);
    else        text(tx.big, 120, 174, &fonts::Font4, 30, 26, COL_ICE);
    text(tx.sub, 120, 206, &fonts::Font2, 15, 16, COL_PINK);
  }
  if (ov & OV_BOOT) {
    text("OTTO", 120, 196, &fonts::Orbitron_Light_24, 24, 24, COL_SILVER);
    text("E420 // 4.2 V8", 120, 218, &fonts::Font2, 13, 16, COL_PINK);
  }

  // ------------------------------------------------- holiday costumes ------
  // Hats (docs/faces/20-holidays-birthday.html, "idle" boards) are drawn in FACE space,
  // so they shrink with the mini face on greeting cards, and ride the rotor offset
  // (rdx/rdy) so the party hat hops with the rotor on braking.
  if (ov & (OV_PARTY_HAT | OV_SANTA_HAT)) {
    // Otto: the hat sits on the piston crown, so it pumps, leans and hops with it
    float topX, topY;
    PP(100, 30, topX, topY);
    float hx = topX - 120, hy = topY - 40;   // hat shapes were drawn for a crown at (~122, 40)
    if (ov & OV_PARTY_HAT) {
      // yellow cone (104,42)(124,4)(142,42), pink stripes + pom, all tilted 8 deg about (122,40)
      const float ca = cosf(8 * DEG_TO_RAD), sa = sinf(8 * DEG_TO_RAD);
      auto rx = [&](float x, float y) { return 122 + (x - 122) * ca - (y - 40) * sa + hx; };
      auto ry = [&](float x, float y) { return 40 + (x - 122) * sa + (y - 40) * ca + hy; };
      canvas.fillTriangle(FX(rx(104, 42)), FY(ry(104, 42)), FX(rx(124, 4)), FY(ry(124, 4)),
                          FX(rx(142, 42)), FY(ry(142, 42)), COL_YELLOW);
      fLine(rx(109, 32), ry(109, 32), rx(137, 32), ry(137, 32), 3, COL_PINK);
      fLine(rx(114, 22), ry(114, 22), rx(132, 22), ry(132, 22), 3, COL_PINK);
      fLine(rx(119, 12), ry(119, 12), rx(128, 12), ry(128, 12), 3, COL_PINK);
      fDot(rx(124, 4), ry(124, 4), 5, COL_PINK);
    } else {
      // pink hat: M98 44 Q116 6 150 12 Q134 26 142 44 Z, white brim + pom
      const int N = 8;
      float px[2 * N + 1], py[2 * N + 1];
      for (int i = 0; i <= N; i++) {            // outer curve 98,44 -> 150,12
        float u = (float)i / N, v = 1 - u;
        px[i] = v * v * 98 + 2 * v * u * 116 + u * u * 150;
        py[i] = v * v * 44 + 2 * v * u * 6 + u * u * 12;
      }
      for (int i = 1; i <= N; i++) {            // inner curve 150,12 -> 142,44
        float u = (float)i / N, v = 1 - u;
        px[N + i] = v * v * 150 + 2 * v * u * 134 + u * u * 142;
        py[N + i] = v * v * 12 + 2 * v * u * 26 + u * u * 44;
      }
      for (int i = 0; i < 2 * N; i++)           // fan from a point inside the hat
        canvas.fillTriangle(FX(122 + hx), FY(34 + hy), FX(px[i] + hx), FY(py[i] + hy),
                            FX(px[i + 1] + hx), FY(py[i + 1] + hy), COL_PINK);
      canvas.fillTriangle(FX(122 + hx), FY(34 + hy), FX(px[2 * N] + hx), FY(py[2 * N] + hy),
                          FX(px[0] + hx), FY(py[0] + hy), COL_PINK);
      uint16_t white = rgb565(201, 211, 222);
      canvas.fillSmoothRoundRect(FX(94 + hx), FY(40 + hy), FW(52), FW(10), FW(5), white);
      fDot(152 + hx, 12 + hy, 6, white);
    }
  }
  if (ov & OV_SNOW) {
    // four flakes drifting down over 3 s, fading in then out
    static const float kSX[] = {50, 190, 70, 176}, kSY[] = {40, 34, 70, 80};
    static const float kSR[] = {2.5f, 2.5f, 2, 2}, kSD[] = {0, 0.6f, 1.2f, 1.8f};
    for (int i = 0; i < 4; i++) {
      float p = fmodf(t + 3 - kSD[i], 3.0f) / 3.0f;
      float a = p < 0.15f ? p / 0.15f : 1 - (p - 0.15f) / 0.85f;
      aDot(kSX[i], kSY[i] - 20 + 50 * p, kSR[i], mix565(C_BG, (uint16_t)rgb565(201, 211, 222), a));
    }
  }
  if (ov & OV_CONFETTI) {
    // Burst of small colored dots; use t for slow drift
    static const float kCX[] = {80, 100, 120, 140, 160, 90, 150, 70, 170, 110};
    static const float kCY[] = {30,  18,  22,  18,  28, 42,  40, 52,  48,  10};
    static const uint16_t kCC[] = {COL_YELLOW, COL_PINK, COL_ICE, COL_YELLOW, COL_PINK,
                                   COL_ICE,   COL_YELLOW, COL_PINK, COL_ICE, COL_YELLOW};
    for (int i = 0; i < 10; i++) {
      float y = kCY[i] + 8 * fmodf(t * 0.5f + i * 0.3f, 1.0f);
      float a = pulse(t + i * 0.2f, 1.5f);
      aDot(kCX[i], y, 4, mix565(C_BG, kCC[i], a));
    }
  }
  if (ov & OV_FIREWORKS) {
    // Firework bursts, matching docs/faces/20-holidays-birthday.html: rays grow from
    // 0.2x to 1.15x over 1.8 s (ease-out), fade in for the first 25 %, then fade out.
    // Bursts are staggered 0.6 s. New Year (has confetti too) and July 4th use their own sets.
    struct Burst { float x, y; uint16_t c; float delay; uint8_t n; int8_t ray[6][2]; };
    static const Burst kNewYear[] = {
      {62, 44, COL_YELLOW, 0.0f, 6, {{0, -14}, {12, -6}, {12, 8}, {0, 14}, {-12, 8}, {-12, -6}}},
      {180, 50, COL_PINK,  0.6f, 6, {{0, -16}, {14, -8}, {14, 8}, {0, 16}, {-14, 8}, {-14, -8}}},
    };
    static const Burst kJuly4[] = {
      {70, 52, COL_PINK,  0.0f, 6, {{0, -20}, {18, -8}, {18, 10}, {0, 20}, {-18, 10}, {-18, -8}}},
      {170, 44, COL_ICE, 0.6f, 6, {{0, -18}, {16, -8}, {16, 10}, {0, 18}, {-16, 10}, {-16, -8}}},
      {120, 30, rgb565(201, 211, 222), 1.2f, 5, {{0, -12}, {11, -6}, {11, 6}, {-11, 6}, {-11, -6}}},
    };
    bool ny = ov & OV_CONFETTI;
    const Burst* bs = ny ? kNewYear : kJuly4;
    int nb = ny ? 2 : 3;
    for (int b = 0; b < nb; b++) {
      float p = fmodf(t + 1.8f - bs[b].delay, 1.8f) / 1.8f;   // 0..1 through the burst
      float e = 1 - (1 - p) * (1 - p);                          // ease-out
      float sc = 0.2f + 0.95f * e;
      float a = p < 0.25f ? p / 0.25f : 1 - (p - 0.25f) / 0.75f;
      if (a < 0.03f) continue;
      uint16_t c = mix565(C_BG, bs[b].c, a);
      for (int k = 0; k < bs[b].n; k++)
        aLine(bs[b].x, bs[b].y, bs[b].x + sc * bs[b].ray[k][0], bs[b].y + sc * bs[b].ray[k][1], 3, c);
    }
  }
  // ------------------------------------------------- everyday extras ------
  if (ov & OV_DOTRING) {                       // dashed gold ring, dots twinkle in turn
    for (int i = 0; i < 64; i++) {
      float a = i * 2 * PI / 64;
      aDot(120 + 108 * cosf(a), 120 + 108 * sinf(a), 2.2f, yellow(pulse(t + i * 0.05f, 1.6f, 0.45f)));
    }
  }
  if (ov & OV_IMPACT) {
    uint16_t c = yellow(pulse(t, 1.2f));
    aLine(182, 40, 182, 50, 3, c); aLine(194, 50, 202, 44, 3, c);
    aLine(196, 62, 206, 62, 3, c); aLine(170, 50, 164, 44, 3, c);
  }
  if (ov & OV_CUP) {
    const float cx = 16;                       // cup sits outside the cylinder wall
    aLine(158 + cx, 78, 160 + cx, 100, 3, COL_YELLOW); aLine(160 + cx, 100, 178 + cx, 100, 3, COL_YELLOW);
    aLine(178 + cx, 100, 180 + cx, 78, 3, COL_YELLOW); aLine(180 + cx, 78, 158 + cx, 78, 3, COL_YELLOW);
    aBez(180 + cx, 82, 190 + cx, 84, 186 + cx, 92, 3, COL_YELLOW); aBez(186 + cx, 92, 184 + cx, 96, 179 + cx, 95, 3, COL_YELLOW);
    for (int i = 0; i < 2; i++) {              // steam rising and fading
      float p = fmodf(t / 1.8f + i * 0.17f, 1.0f), dy = -10 * p;
      uint16_t c = mix565(C_BG, COL_GRAY, p < 0.3f ? p / 0.3f : 1 - (p - 0.3f) / 0.7f);
      float x = 180 + 8 * i;
      aBez(x, 72 + dy, x - 4, 66 + dy, x, 60 + dy, 2.5f, c);
    }
  }
  if (ov & OV_HEAT) {                          // three wavy lines bobbing, two sweat drops
    static const float kX[] = {92, 112, 132}, kY[] = {42, 32, 42};
    for (int i = 0; i < 3; i++) {
      float y = kY[i] - 2 - 2 * sinf((t - i * 0.3f) * 2 * PI / 1.2f), x = kX[i];
      aBez(x, y, x + 6, y - 6, x + 12, y, 3, COL_YELLOW);
      aBez(x + 12, y, x + 18, y + 6, x + 24, y, 3, COL_YELLOW);
    }
    canvas.fillTriangle(AX(146.5f), AY(85), AX(153.5f), AY(85), AX(150), AY(76), COL_ICE);
    aDot(150, 86, 3.5f, COL_ICE);
    canvas.fillTriangle(AX(85), AY(91), AX(91), AY(91), AX(88), AY(84), COL_ICE);
    aDot(88, 91, 3, COL_ICE);
  }
  if (ov & (OV_WREATH | OV_WREATH_ALLDAY)) {
    // 12 gold leaves (design ellipses rx 8, ry 3.5); the all-day version is scaled up around the full face
    static const float kL[12][3] = {{68, 134, 240}, {61, 114, 260}, {61, 94, 280}, {68, 74, 300}, {81, 58, 320}, {99, 48, 340},
                                    {172, 134, 120}, {179, 114, 100}, {179, 94, 80}, {172, 74, 60}, {159, 58, 40}, {141, 48, 20}};
    bool big = !(ov & OV_WREATH);
    float k = big ? 1.62f : 1.0f, ox = 120, oy = big ? 122 : 95, lk = big ? 1.3f : 1.0f;
    for (int i = 0; i < 12; i++) {
      float cx = ox + (kL[i][0] - 120) * k, cy = oy + (kL[i][1] - 95) * k;
      float a = kL[i][2] * DEG_TO_RAD, ca = cosf(a), sa = sinf(a), rx = 8 * lk, ry = 3.5f * lk;
      float px[8], py[8];
      for (int j = 0; j < 8; j++) {            // octagon approximating the ellipse
        float u = j * PI / 4, ex = rx * cosf(u), ey = ry * sinf(u);
        px[j] = AX(cx + ex * ca - ey * sa); py[j] = AY(cy + ex * sa + ey * ca);
      }
      for (int j = 1; j < 7; j++) canvas.fillTriangle(px[0], py[0], px[j], py[j], px[j + 1], py[j + 1], COL_YELLOW);
    }
    if (!big) {
      // pink bow under the face, then the text
      canvas.fillTriangle(AX(110), AY(146), AX(120), AY(140), AX(120), AY(150), COL_PINK);
      canvas.fillTriangle(AX(110), AY(146), AX(120), AY(150), AX(114), AY(156), COL_PINK);
      canvas.fillTriangle(AX(130), AY(146), AX(120), AY(140), AX(120), AY(150), COL_PINK);
      canvas.fillTriangle(AX(130), AY(146), AX(120), AY(150), AX(126), AY(156), COL_PINK);
      text("AUTO-GEBURTSTAG", 120, 182, &fonts::Font4, 16, 26, COL_YELLOW);   // the car's birthday
      text("29.01.1886", 120, 206, &fonts::Font2, 16, 16, COL_PINK);
    }
  }
  if (ov & OV_CARD) {                          // car show spec card
    canvas.fillSmoothRoundRect(AX(44), AY(98), AW(152), AW(94), AW(8), COL_PANEL);
    for (int i = 0; i < 2; i++)
      canvas.drawRoundRect(AX(44) + i, AY(98) + i, AW(152) - 2 * i, AW(94) - 2 * i, AW(8), rgb565(58, 68, 82));
    text(tx.label, 120, 124, &fonts::Font4, 22, 26, COL_ICE);
    text(tx.big, 120, 150, &fonts::Font4, 17, 26, COL_YELLOW);
    text(tx.sub, 120, 176, &fonts::Font4, 17, 26, pink(pulse(t, 1.2f)));
  }
  if (ov & OV_LANTERN) {
    // Jack-o'-lantern piston: the body glows yellow (drawPiston), ice-blue stem on the crown,
    // flickering triangle eyes + jagged grin on the piston face. Normal eyes are hidden (eyeBright 0).
    uint16_t c = yellow(fmodf(t, 0.5f) < 0.25f ? 1.0f : 0.75f);   // candle flicker, 0.5 s steps
    pBez(100, 30, 102, 20, 110, 18, 4, COL_ICE);
    pTri(80, 82, 88, 68, 96, 82, c);
    pTri(104, 82, 112, 68, 120, 82, c);
    static const float gx[] = {82, 88, 94, 100, 106, 112, 118, 114, 106, 100, 94, 86};
    static const float gy[] = {86, 91, 86, 91, 86, 91, 86, 96, 93, 98, 93, 96};
    for (int i = 0; i < 12; i++) {
      int j = (i + 1) % 12;
      pTri(100, 91, gx[i], gy[i], gx[j], gy[j], c);
    }
  }
}

// ------------------------------------------------------- public helper ---
void faceText(const char* s, float x, float y, int fontSize, float designH, uint16_t color, Align a) {
  const lgfx::IFont* font = fontSize == 7 ? (const lgfx::IFont*)&fonts::Font7
                          : fontSize == 4 ? (const lgfx::IFont*)&fonts::Font4
                                          : (const lgfx::IFont*)&fonts::Font2;
  float fontH = fontSize == 7 ? 48 : fontSize == 4 ? 26 : 16;
  fitText(s, x, y, font, designH, fontH, a == Align::Left ? -1 : a == Align::Right ? 1 : 0);
  canvas.setTextColor(color);
  canvas.setTextDatum(a == Align::Left ? textdatum_t::middle_left
                    : a == Align::Right ? textdatum_t::middle_right : textdatum_t::middle_center);
  canvas.drawString(s, AX(x), AY(y));
}

// -------------------------------------------------------------- render ---
// `perf`: time per part of the frame (us, summed): clear, cylinder, piston + mouth, brows + eyes, overlays
uint32_t g_perfPart[5] = {0, 0, 0, 0, 0};

void faceRender(const FaceState& f, const FaceText& tx, float crankAngleDeg, float blinkOpen, float glow,
                float t) {
  g_fcx = f.fcx; g_fcy = f.fcy; g_fs = f.fscale; g_glow = glow;
  uint32_t pt = micros(), pn;
  canvas.fillScreen(COL_BG);
  pn = micros(); g_perfPart[0] += pn - pt; pt = pn;

  // piston pose: stroke from the crank, lean from tilt, size, whole-character offset
  s_dx = f.rdx; s_dy = f.rdy; s_hs = f.rscale;
  float lean = constrain(f.tilt * 0.6f, -18.0f, 18.0f) * DEG_TO_RAD;
  s_lc = cosf(lean); s_ls = sinf(lean);
  bool lantern = f.ov & OV_LANTERN;
  crankGeometry(lantern ? 0 : crankAngleDeg, lantern ? 0 : f.rest);

  if (lantern) {
    s_body = yellow(f.rbright);
    s_bodyGlow = mix565(C_BG, C_YELLOW, 0.45f * g_glow * f.rbright);
    s_hue = COL_YELLOW; s_bright = f.rbright; s_glowK = 0.45f * g_glow;
  } else if (f.gray) {
    s_body = mix565(C_BG, COL_SLEEPY, fmaxf(0.6f, f.rbright));
    s_bodyGlow = COL_BG;
    s_hue = COL_SLEEPY; s_bright = fmaxf(0.6f, f.rbright); s_glowK = 0;
  } else {
    s_body = mix565(C_BG, C_SILVER, f.rbright);
    s_bodyGlow = mix565(C_BG, C_SILVER, 0.22f * g_glow * f.rbright);
    s_hue = COL_SILVER; s_bright = f.rbright; s_glowK = 0.22f * g_glow;
  }
  s_eye = mix565(C_BG, C_ICE, f.eyeBright);
  s_eyeGlow = mix565(C_BG, C_ICE, 0.32f * glow * f.eyeBright);

  if (f.ov & OV_HOUSING) drawCylinder();
  pn = micros(); g_perfPart[1] += pn - pt; pt = pn;
  if (f.rbright > 0.01f) {
    drawPiston(f);
    if (f.drawFrac > 0.99f && f.eyeBright > 0.01f && !lantern) drawMouth(f.mouth, t, f.ov & OV_YAWN);
  }

  pn = micros(); g_perfPart[2] += pn - pt; pt = pn;
  if (f.eyeBright > 0.01f && f.drawFrac > 0.99f) {
    Brow brow = f.brow;
    if (f.ov & OV_BROWS_DET) brow = Brow::Determined;
    if (f.ov & OV_BROWS_SAD) brow = Brow::Sad;
    drawBrows(brow, f.eyeDY * 0.25f);
    // eye offsets are in Rotor's bigger-face units: scale them so the eyes stay on the piston
    float ex = 0.45f * f.eyeDX, ey = 0.45f * f.eyeDY, lx, ly, rx, ry;
    PP(88 + ex, 77 + ey, lx, ly);
    PP(112 + ex, 77 + ey, rx, ry);
    float sc = 0.75f * s_hs, r = f.eyeR * sc;
    drawEye(f.eye, lx, ly, r, blinkOpen, true, s_eye, s_eyeGlow, sc);
    drawEye(f.eye, rx, ry, r, blinkOpen, false, s_eye, s_eyeGlow, sc);
  }
  pn = micros(); g_perfPart[3] += pn - pt; pt = pn;
  drawOverlays(f, tx, t);
  g_perfPart[4] += micros() - pt;
}
