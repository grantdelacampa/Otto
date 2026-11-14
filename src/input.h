#pragma once
#include <Arduino.h>

// Side buttons (touchscreen support can be added later):
//   BOOT tap        -> boop (happy + chirp)
//   BOOT hold 2 s   -> recalibrate mount
//   PWR tap         -> show battery level
//   PWR hold 3 s    -> power off (on battery)
void inputUpdate(uint32_t now);
