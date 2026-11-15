#pragma once
#include <Arduino.h>

// Otto's face is drawn in a 240x240 "design space" and scaled to the panel (LCD_W, 412x412 on
// the 1.46B). Otto is a piston in its cylinder (docs/design/Otto.dc.html): the piston's stroke
// follows a crankshaft angle, so "spin" speeds are crank speeds (idle = a slow pump).

enum class Eye : uint8_t { Dot, Wide, Happy, Content, Closed, Cross, Spiral, Chevron, Half, Wavy, Uneven,
                           Ahem };   // Ahem: one eye open, the other half-lidded (disapproves)

// Eyebrows and mouth (drawn on the piston; Rotor had neither)
enum class Brow : uint8_t { None, Flat, Raised, Pleased, Determined, Angry, Sad, Ahem, Uneven };
enum class Mouth : uint8_t { None, Flat, Small, Smile, Pleased, Frown, Grit, O, Talk };

enum : uint64_t {
  OV_HOUSING    = 1ull << 0,   // cylinder walls + crank case
  OV_ZZ         = 1ull << 1,
  OV_SOUND      = 1ull << 2,   // sound arcs both sides (listening)
  OV_THINKDOTS  = 1ull << 3,
  OV_NOTE       = 1ull << 4,
  OV_SPEEDTOP   = 1ull << 5,   // braking
  OV_TRAIL      = 1ull << 6,   // accelerating
  OV_ARCS_R     = 1ull << 7,   // left turn (arcs on the right)
  OV_ARCS_L     = 1ull << 8,   // right turn
  OV_JOLT       = 1ull << 9,
  OV_ORBIT      = 1ull << 10,
  OV_BADGE42    = 1ull << 11,  // "4.2" V8 badge
  OV_RING       = 1ull << 12,  // calibrate progress
  OV_VOLUME     = 1ull << 13,
  OV_QUESTION   = 1ull << 14,
  OV_MOUTHWAVE  = 1ull << 15,
  OV_YAWN       = 1ull << 16,
  OV_BATTERY    = 1ull << 17,
  OV_AHEM       = 1ull << 18,  // "AHEM." (disapproves)
  OV_SWEAT      = 1ull << 19,
  OV_TWINKLE    = 1ull << 20,  // two twinkling stars (sehr gut)
  OV_CHECK      = 1ull << 21,  // check mark + caption text
  OV_CAPTION    = 1ull << 22,
  OV_SHIFT      = 1ull << 23,  // shift light: flashing ring (OBD rpm >= shiftRpm)
  OV_SIDELINES  = 1ull << 24,  // speed lines on both sides (launch)
  OV_BROWS_SAD  = 1ull << 25,
  OV_BROWS_DET  = 1ull << 26,
  OV_GREETING   = 1ull << 27,
  OV_ANSWER     = 1ull << 28,  // card: label / big / sub
  OV_BOOT       = 1ull << 29,
  OV_SPEAK      = 1ull << 30,  // small speaker arcs (greeting)
  OV_DOTS3      = 1ull << 31,  // "..." (bored)
  OV_HEARTS     = 1ull << 32,  // petting
  OV_SPARKLE    = 1ull << 33,  // tap happy
  OV_EXCLAIM    = 1ull << 34,  // startled
  OV_STEAM      = 1ull << 35,  // grumpy
  OV_LOCK       = 1ull << 36,  // menu locked
  OV_PUFFS      = 1ull << 37,  // exhaust puffs (rev)
  OV_THERMO     = 1ull << 38,  // coolant thermometer (fill = bat, color = batColor)
  OV_LIGHTS     = 1ull << 39,  // launch staging lights
  OV_RUMBLE     = 1ull << 40,  // engine start zigzags
  OV_UPDATE     = 1ull << 41,  // firmware update ring + text
  OV_SPEEDRING  = 1ull << 42,  // yellow progress ring (0-60)
  OV_OPENRING   = 1ull << 43,  // ice-blue progress ring (menu opening)
  OV_PARTY_HAT  = 1ull << 44,  // birthday party hat (stays upright on housing)
  OV_SANTA_HAT  = 1ull << 45,  // Christmas Santa hat
  OV_CONFETTI   = 1ull << 46,  // birthday / New Year confetti burst
  OV_FIREWORKS  = 1ull << 47,  // 4th of July fireworks burst
  OV_LANTERN    = 1ull << 48,  // Halloween: jack-o'-lantern piston
  OV_SNOW       = 1ull << 49,  // Christmas falling snow
  OV_TREE       = 1ull << 50,  // drag-strip Christmas tree (bulbs from FaceText.tree)
  OV_STREAKS    = 1ull << 51,  // launch speed streaks + GO!
  OV_SLIP       = 1ull << 52,  // drag timeslip (FaceText.slip)
  // everyday extras (docs/faces/22-everyday.html)
  OV_DOTRING    = 1ull << 53,  // milestone: dotted gold ring around the edge
  OV_CUP        = 1ull << 54,  // break time: cup + steam
  OV_HEAT       = 1ull << 55,  // hot cabin: heat waves + sweat drops
  OV_WREATH     = 1ull << 56,  // car's birthday (Jan 29, 1886) greeting: laurel wreath + bow + text
  OV_WREATH_ALLDAY = 1ull << 57,  // car's birthday costume: bigger wreath around the full face
  OV_CARD       = 1ull << 58,  // car show spec card (FaceText label / big / sub)
  OV_IMPACT     = 1ull << 59,  // guard: impact lines by the face
};

struct FaceState {
  // smoothly interpolated
  float spin = 140;       // crank speed, deg/s (140 = one stroke every ~2.6 s, the design's idle pump)
  float tilt = 0;         // lean of the piston, deg (turns, wobble, dizzy)
  float rscale = 1;       // piston size
  float rdx = 0, rdy = 0; // whole-character offset
  float rbright = 1;      // body brightness 0..1
  float rest = 0;         // 0 = pumping with the crank, 1 = settled at the bottom of the stroke (asleep)
  float eyeR = 8;
  float eyeDX = 0, eyeDY = 0;
  float eyeBright = 1;
  float fcx = 120, fcy = 120, fscale = 1;  // whole-face transform (mini face for cards)
  float ring = 0;         // progress rings 0..1
  float bat = 0.7f;       // battery icon fill 0..1
  float vol = 50;         // volume 0..100
  float drawFrac = 1;     // how much of the piston outline to draw (boot animation)
  // discrete
  Eye eye = Eye::Dot;
  Brow brow = Brow::Flat;
  Mouth mouth = Mouth::Flat;
  bool gray = false;      // sleepy gray body, no glow
  uint64_t ov = OV_HOUSING;
  bool dashed = false;
  uint16_t captionColor = 0;
  uint16_t batColor = 0;
};

// Drag tree bulbs (FaceText.tree)
enum : uint8_t { TREE_PRESTAGE = 1, TREE_STAGE = 2, TREE_A1 = 4, TREE_A2 = 8, TREE_A3 = 16, TREE_GREEN = 32, TREE_RED = 64 };

struct FaceText {
  uint8_t tree = 0;       // TREE_* bits
  char slip[4][10] = {};  // timeslip values: R/T, 0-30, 0-60, peak g
  char caption[32] = "";
  char label[24] = "";    // answer card
  char big[16] = "";
  char sub[28] = "";
  char greet[48] = "";    // greeting, German (UTF-8: umlauts are 2 bytes)
  char gloss[28] = "";    // small line under it: the English meaning
};

void faceLerp(FaceState& cur, const FaceState& target, float k);   // k = 0..1 per frame
void faceRender(const FaceState& f, const FaceText& txt, float crankAngleDeg, float blinkOpen,
                float glow, float t);
bool faceDirtyRect(int& x, int& y, int& w, int& h);   // the area the last faceRender changed; false = whole screen
void faceForceFull();                                   // next frame redraws everything (something else drew on the canvas)

// Text helper for other screens (menu): design-space coordinates, centered unless left/right
enum class Align : uint8_t { Center, Left, Right };
void faceText(const char* s, float x, float y, int fontSize, float designH, uint16_t color,
              Align a = Align::Center);   // fontSize: 2 = small, 4 = medium, 7 = 7-segment digits
