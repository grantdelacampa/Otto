#pragma once
#include <Arduino.h>

// Text command protocol, shared by BLE (CMD characteristic) and the USB serial console.
// One command per write/line. See README for the full list.

static const uint16_t SRC_SERIAL = 0xFFFF;

// Vehicle speed from an outside source via the `speed <mph>` command. Otto has no car radio,
// so nothing sends it yet (bench: tools/fake_gps.py); the 0-60 timer and trip miles use it if it
// ever arrives (e.g. a future OBD-II or GPS add-on), and otherwise run without it.
struct SpeedInput {
  float mph = 0;
  uint32_t at = 0;            // millis() of the last update (0 = never)
};
extern SpeedInput g_speed;

void   handleCommand(const char* line, uint16_t source);
size_t buildTelemetry(char* buf, size_t n);
