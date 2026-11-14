#pragma once
#include <Arduino.h>

// Idle vibration test (TODO "Idle vibration test"): sample the IMU at its ~900 Hz data rate for ~2.3 s,
// FFT it, and report the strongest frequencies, the V8 firing frequency -> RPM, and the
// half-order (misfire) band. Decides whether vibration-only idle health is viable.
//
//   mode ""    : summary lines (+ spectrum chunks for the tuning page over BLE)
//   mode "csv" : also print the spectrum as hz,g lines (serial)
//   mode "raw" : also print the raw samples as ms,ax,ay,az lines (serial)
//   mode "tone [Hz]": bench self-test, plays a steady tone (default 200 Hz) during the capture
void vibcapRun(const char* mode, uint16_t src, bool serial, void (*reply)(uint16_t, const char*));
