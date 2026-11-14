#pragma once
#include <Arduino.h>

// Drive recorder: 1 record per second with the peak values seen in that second.
// Keeps the last ~2 hours of awake time (across drives), saved to flash on sleep.

struct LogRec {
  uint16_t brake;  // g * 1000
  uint16_t accel;  // g * 1000
  uint16_t lat;    // g * 1000
  uint16_t yaw;    // deg/s * 10
  uint16_t bump;   // g * 1000
};

struct Peaks { float brake = 0, accel = 0, lat = 0, yaw = 0, bump = 0; };

void   recorderInit();
void   recorderSample(uint32_t now, float brake, float accel, float lat, float yaw, float bump);
void   recorderSave();
void   recorderResetPeaks();
Peaks  recorderPeaks();         // peaks of the current drive, or the last one
size_t recorderCount();
LogRec recorderGet(size_t i);   // 0 = oldest
