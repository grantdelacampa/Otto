#pragma once
#include <Arduino.h>

// Drag race + 0-60 timer.
//  - Armed (menu / radio / tuning page / "launch arm"): IKUZO, pre-stage, staged, Christmas tree
//    (Sportsman or PRO TREE), green or RED LIGHT, reaction time from the motion chip. With radio GPS
//    ("speed <mph>", ~5-10 Hz) a live 0-60 run and a timeslip; without GPS a reaction-time slip.
//  - Not armed: if LAUNCH is on, any clean run from a stop that reaches 60 in
//    under 15 s still shows the result card.
void  launchInit();
void  launchArm();
void  launchUpdate(uint32_t now);
float launchBestRt();        // seconds, 0 = none
float launchBest();          // seconds, 0 = none
void  launchResetBest();
bool  launchRunning();
