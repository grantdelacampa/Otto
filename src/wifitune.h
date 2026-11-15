#pragma once
#include <Arduino.h>

// Wi-Fi tuning mode: Otto opens his own Wi-Fi network ("OTTO-xxxx") and serves the tuning page,
// so a phone can tune him in plain Safari / Chrome (no Bluetooth, no app, no internet).
// On demand only (menu / `wifi on`): voice pauses while it runs (core 0 + internal RAM), and it
// switches itself off after 10 minutes without a page request, on `wifi off`, or before sleep.

void wifiTuneStart();
void wifiTuneStop();
void wifiTuneUpdate(uint32_t now);
bool wifiTuneActive();
void wifiTuneInfo(char* b, size_t n);   // status line for `wifi`
const char* wifiTuneSsid();
const char* wifiTunePass();
