#pragma once
#include <Arduino.h>
#include "face.h"

// Every expression Otto can show. Names (lowercase) are used by the
// "expr <name>" command and in telemetry.
enum class Expr : uint8_t {
  Idle, Bored, MusicBob,
  Listening, Thinking, Speaking,
  Happy, Sleepy, Volume, Confused, Answer,
  Braking, TurnLeft, TurnRight, Accel, Bump, Dizzy, V8, Calibrate,
  Boot, Charging, LowBattery, BatteryLevel, ChargeSoon, Charged, Overheat, Sleep,
  Greeting, Boop,
  TapHappy, Wobble, Startled, Pet, Grumpy, Spinning, MenuOpening, MenuLocked, Saved, SehrGut,
  EngineOn, Rev, LaunchReady, LaunchRun, LaunchResult, LaunchTree, LaunchGo, LaunchRed, LaunchRecord, Summary, Updating, HotOff,
  HolidayBirthday, HolidayNewYear, HolidayJuly4, HolidayHalloween, HolidayChristmas,
  Guard, MissedYou, Milestone, BreakTime, HotCabin, ColdCabin, ShowCard, HolidayPatent,
  WarmUp, Warm, EngineHot, CheckEngine,   // OBD-II (obd.cpp)
  COUNT
};

const char* exprName(Expr e);
bool exprFromName(const char* name, Expr& out);

struct BrainCtx {
  bool  playing = false;
  float bpm = 120;
  float stoppedSecs = 0;        // vehicle stopped (engine may be running)
  bool  hadDriven = false;
  bool  night = false;
  int   batPct = 70;
  float tempC = 25;
  float calibProgress = 0;
  float lon = 0;                // current longitudinal g, for captions
  float rpm = 0;                // engine rpm from OBD (0 = unknown / not linked)
  float coolantC = -100;        // coolant from OBD (-100 = unknown)
};

void      brainInit();
void      brainEvent(Expr e, uint32_t durMs);                 // one-shot with default priority
void      brainEventPrio(Expr e, uint32_t durMs, int prio);
void      brainVoice(Expr e);                                  // Listening/Thinking/Speaking, Idle = clear
void      brainSetGreeting(const char* greet, const char* gloss, Eye eye);   // German + English gloss
void      brainSetAnswer(const char* label, const char* big, const char* sub);
void      brainSetVolume(int v);
void      brainSetCaption(const char* text);

// Extra crank speed from touch swipes and revs (deg/s), decays on its own.
void      brainSpinImpulse(float degPerSec);
float     brainUserSpin();                     // current extra spin (deg/s)
void      brainSpinDrag(float degPerSec);               // finger turning the crank: exactly this fast
void      brainSpinRelease(float degPerSec, bool dizzy); // let go: coast from here (dizzy when it stops)
float     brainUserSpinStep(float dt);   // call once per frame; returns current extra spin
void      brainSetTree(uint8_t bulbs);   // drag tree: TREE_* bits (face.h)
void      brainSetSlip(const char* rt, const char* t30, const char* t60, const char* peak);  // timeslip values
void      brainSetRing(float p);         // progress for LaunchRun / Updating / MenuOpening
Expr      brainCurrent();
void      brainUpdate(uint32_t now, const BrainCtx& ctx, FaceState& target, float& blinkOpen);
FaceText& brainText();
