#pragma once
#include <Arduino.h>

// SPD2010 touch on the main I2C bus (first finger only), plus a small gesture recognizer:
//   tap                      -> random reaction (5+ taps in 2 s -> grumpy)
//   hold 0.8 s               -> petting, for as long as the finger stays
//   drag / flick             -> turn the crank: the crankshaft follows the finger around the centre, then coasts
//   tap, then tap-and-hold   -> open the config menu (hold 0.6 s, only when stopped)
// When the menu is open, taps and swipes go to the menu instead (close it with EXIT).

struct TouchPoint { int16_t x, y; };

bool touchInit();
void touchUpdate(uint32_t now);
bool touchActive();                 // a finger is down right now
void touchPrepareSleep();           // before deep sleep: let the chip idle so a touch can wake us
void touchResume();                 // after touchPrepareSleep() when not sleeping after all (debug)
int  touchDebugRead(TouchPoint* pts, int maxPts);  // raw read for the "touch" console command
int  touchRawRead(TouchPoint* pts);                // `touch raw`: read now (ignores INT), prints the status
