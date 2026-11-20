#pragma once
#include <Arduino.h>

// Bench power test (`pwrtest start`): on battery, switch one thing at a time (backlight, frame rate,
// CPU clock, voice, Bluetooth) and average the battery voltage in each step. The cell sags in
// proportion to the current, so the drop vs the baseline steps on either side ranks what costs
// what. Results go to /pwrtest.csv (`pwrtest` prints them); Otto restarts when it's done.

void pwrtestStart(uint32_t delayMs);   // waits this long (time to unplug USB), then runs
void pwrtestStop();
bool pwrtestActive();
void pwrtestUpdate(uint32_t now);
void pwrtestReport(uint16_t src, void (*reply)(uint16_t, const char*));

extern int g_ptBri;       // -1 = normal brightness, else percent (main.cpp frame())
extern int g_ptFrameMs;   // 0 = normal frame interval, else ms between frames
