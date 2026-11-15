#pragma once
#include <Arduino.h>

// Text command protocol, shared by BLE (CMD characteristic) and the USB serial console.
// One command per write/line. See README for the full list.

static const uint16_t SRC_SERIAL = 0xFFFF;
static const uint16_t SRC_HTTP   = 0xFFFD;   // Wi-Fi tuning page (wifitune.cpp captures the replies)

// Vehicle speed from an outside source via the `speed <mph>` command. Otto has no car radio,
// so nothing sends it yet (bench: tools/fake_gps.py); the 0-60 timer and trip miles use it if it
// ever arrives (e.g. a future OBD-II or GPS add-on), and otherwise run without it.
struct SpeedInput {
  float mph = 0;
  uint32_t at = 0;            // millis() of the last update (0 = never)
};
extern SpeedInput g_speed;

// `perf boot`: the first frames after power-on (main.cpp fills it)
struct BootFrame { uint32_t t; uint16_t ms10; uint8_t expr; };   // millis, draw+push in 0.1 ms, Expr
static const int BOOT_LOG = 120;
extern BootFrame g_bootLog[BOOT_LOG];
extern int g_bootLogN;

void   handleCommand(const char* line, uint16_t source);
size_t buildTelemetry(char* buf, size_t n);
