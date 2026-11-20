#pragma once
#include <Arduino.h>

// Everything the tuning page can change. Saved to flash (NVS).
struct Settings {
  // Motion thresholds
  float brake = 0.35f;   // g, deceleration
  float accel = 0.25f;   // g
  float turn  = 25.0f;   // deg/s yaw rate
  float bump  = 0.80f;   // g, vertical spike
  float v8    = 0.45f;   // g, launch for V8 mode
  // Personality
  int  activity = 5;     // 1 calm .. 10 hyper
  int  sleepMin = 2;     // minutes of stillness (tuning page not connected) before deep sleep
  bool v8On = true, bobOn = true, boredOn = true, yawnOn = false, greetOn = true;
  // Touch
  bool touchWake = true; // a touch wakes Otto from sleep
  bool motionWake = true; // peek at the gyro every 4 s while asleep (off = buttons/touch only, saves battery)
  // Car extras
  bool  revOn = true, launchOn = true, summaryOn = true;
  bool  voiceOn = true;      // offline voice commands ("Hi ESP" + command)
  bool  treePro = false;     // drag tree: false = Sportsman (ambers 0.5 s apart), true = Pro (all at once, 0.4 s)
  float engineVib = 0.010f;  // g RMS vibration that counts as "engine running" (tune per car)
  float revRatio = 2.0f;     // a rev = vibration this many times the idle level
  float hotOffC = 65.0f;     // above this for 10 s: show TOO HOT and sleep
  // Display
  int  dayBri = 10, nightBri = 10, glow = 70;   // percent (owner, Nov 19 2026: 10 % day + night for battery)
  // Sound
  int  volume = 60;      // percent
  // Mounting / calibration
  int   fwdSign = 1;     // flip if braking shows as accelerating
  bool  calibrated = false;
  float upX = 0, upY = 0, upZ = 1;
  // Safety
  float overheatC = 55.0f;
  // Holidays + birthday
  bool  holidaysOn    = true;
  int   birthdayMonth = 0;   // 0 = not set
  int   birthdayDay   = 0;
  // Everyday extras (Rotor's designs, redrawn for Otto)
  bool  guardOn = true;      // log bumps while parked/asleep, report them on the next wake
  bool  breakOn = true;      // "Pause?" after ~2 h of driving, at a stop
  bool  cabinOn = true;      // hot / cold cabin face on wake (motion-chip temperature)
  bool  milestoneOn = true;  // drive #10, #25, #50, #100 ... celebrations; 7+ days parked greeting
  int   hotC = 40;           // cabin at/above this on wake -> "Heiß!"
  int   coldC = 5;           // cabin at/below this on wake -> "Kalt!"
  int   showMin = 60;        // car show mode turns itself off after this many minutes
  char  showL1[16] = "1997 E420";       // car show spec card, line 1
  char  showL2[24] = "W210 - 4.2 V8";   // line 2
  // OBD-II (BLE ELM327 adapter, obd.cpp)
  bool  obdOn = false;       // look for / use the adapter (off until one is bought)
  int   shiftRpm = 5800;     // shift light at this rpm (0 = off)
  int   coldCoolC = 50;      // coolant below this after a start -> warm-up face
  int   hotCoolC = 115;      // coolant at/above this -> engine hot face
  // --- added in SETTINGS_VERSION 3: keep new fields at the end (settingsLoad carries v2 over) ---
  bool  wirelessOn = true;   // Bluetooth + Wi-Fi tuning; off = no radio at all
  // --- added in SETTINGS_VERSION 4 ---
  bool  driveLogOn = true;   // drivelog: a diagnostics line every 2 s while moving (menu DRIVE LOG)
  // ENGINE LEARN (menu): measured at idle in P, then with the engine off. 0 = not learned yet:
  // engine on = engineVib, off below 0.3 x engineVib, quiet = the defaults below.
  float engOffVib = 0;       // g RMS: below this (with a still body) the engine counts as off
  float idleVib = 0;         // g RMS: this car's idle, the base a rev has to rise above
  float quietGyro = 0.6f;    // deg/s: rotation wobble below this = really stopped (not cruising)
  float quietRoad = 0.012f;  // g RMS: vertical body motion below this = really stopped
  // --- added in SETTINGS_VERSION 5: what quietGyro / quietRoad are worked out from (0 = not learned)
  float idleGyro = 0, idleRoad = 0;      // ENGINE LEARN: wobble (dps) / road (g) at idle in P
  float cruiseGyro = 0, cruiseRoad = 0;  // "learn the road" (voice): the quiet end of steady cruising
};

extern Settings g_set;

void   settingsLoad();
void   settingsSave();
String settingsToJson();
bool   settingsApplyJson(const char* json);  // partial JSON merge, saves
void   settingsReset();
