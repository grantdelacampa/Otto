#pragma once
#include <Arduino.h>

// Everyday extras (docs/faces/22-everyday.html): guard mode, missed you, milestones, break time,
// hot / cold cabin, car show mode. Each follows its setting (guardOn, breakOn, cabinOn,
// milestoneOn; missed-you also needs greetOn).

// --- guard mode: called from the 4 s sleep peek when the car rocked and settled again
void extrasGuardBump();

// --- wake / sleep
void extrasOnWake(bool coldBoot);   // after motionInit: reads the cabin temperature, checks guard + days parked
bool extrasGreet();                 // instead of the normal greeting (7+ days parked); false = not taken
void extrasOnSleep();               // remember when we were last awake
bool extrasCardPending();           // guard / cabin card still waiting to be shown

// --- main loop: shows pending cards when `ready` (greeting done, idle), break time, show mode
void extrasUpdate(uint32_t now, bool ready);

// --- milestones: trip.cpp calls this when a drive (3+ min) finishes
void extrasDriveDone();
bool extrasShowMilestone();         // shows a pending milestone now (true if it did)
uint32_t extrasDriveCount();

// --- car show mode
void extrasShow(bool on);
bool extrasShowOn();

// --- bench helpers (`extras ...` command)
void extrasDebug(const char* what, char* reply, size_t n);
