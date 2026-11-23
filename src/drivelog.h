#pragma once
#include <Arduino.h>

// Drive diagnostics: one line every 2 s while Otto is awake and not sitting still, so the engine /
// stopped / rev / music thresholds can be tuned from a real drive (`drivelog` prints it).
// /drive.csv, rolls over to /drive.old at 192 KB (~3 h of driving each). Buffered, written every 30 s.

void drivelogUpdate(uint32_t now);
void drivelogEvent(const char* what);   // a marker line (e.g. "summary", "rev")
void drivelogFlush();                   // before sleep
void drivelogDump(uint16_t src, void (*reply)(uint16_t, const char*));
void drivelogClear();
