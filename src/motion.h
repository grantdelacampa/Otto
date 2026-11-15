#pragma once
#include <Arduino.h>

// QMI8658 6-axis IMU -> car-frame motion (longitudinal / lateral / vertical g,
// yaw rate), event detection and calibration.

struct MotionState {
  bool  ok = false;
  float lon = 0, lat = 0, vert = 0;   // g, car frame (lon + = accelerating, lat + = left)
  float yaw = 0;                      // deg/s, + = turning left
  float tempC = 25;                   // IMU die temperature (close to case temperature)
  float stillSecs = 0;                // truly still (engine off / parked)
  float stoppedSecs = 0;              // not moving, engine vibration allowed
  bool  hadDriven = false;
  bool  calibrating = false;
  float calibProgress = 0;
  float vib = 0;                      // g RMS engine/road vibration
  bool  engineOn = false;
  bool  engineStarted = false;        // edge flags: main.cpp clears them after handling
  bool  engineStopped = false;
};

extern MotionState g_motion;

bool motionInit();
void motionUpdate(uint32_t now);       // call every loop; samples at ~100 Hz
void motionStartCalibration();
void motionFlipForward();

// For the deep-sleep wake check: find the IMU without reconfiguring it, then
// read accel samples in g (sensor frame)
bool motionProbeOnly();
bool motionReadAccel(float& x, float& y, float& z);
bool motionReadTemp(float& c);         // IMU die temperature, deg C
void motionLowPowerTest(uint8_t ctrl2, uint8_t ctrl5);  // debug (imulp)
void motionWomCheck(bool reset, int secs);  // debug (womcheck): the sleep peek on the bench
void motionWomTest(uint8_t mg, uint8_t ctrl2, int secs);  // debug (wom): arm wake-on-motion, poll its flags
int  motionVibCapture(float* ax, float* ay, float* az, int n, float rateHz, float& actualHz);  // vibcap
void motionLowPower();                 // accel only, low rate, before deep sleep
void motionLeaveWom();   // after wake-on-motion: soft reset + low-power readings (else the data stays frozen)
// Wake-on-motion (sleep): the chip watches continuously and latches a flag (STATUS1 bit 2) that the
// 4 s peek reads. Its INT only reaches the IO expander, so it can't wake the S3 itself.
// While armed the accel registers stop updating: call motionLeaveWom() (+ ~120 ms) before reading.
bool motionArmWom(uint8_t mg);
bool motionWomFired();                 // read + clear the latched flag
