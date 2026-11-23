#pragma once
#include <Arduino.h>

void  powerInit();
void  powerUpdate(uint32_t now);
float powerVolts();
int   powerPct();
// Battery log (for measuring real runtime without a meter): one line per boot / wake / sleep /
// power-off and every 5 min awake, in /littlefs/batlog.csv as  unix,mV,pct,event
void  powerLog(const char* event);
void  powerLogAt(const char* event, int mv);   // log a voltage read earlier (the last sleep peek's)
void  powerLogDump(uint16_t src, void (*reply)(uint16_t, const char*), float capMah);  // + drain summary
void  powerLogClear();
bool  powerCharging();
bool  powerFull();        // charging and the voltage has stopped rising at the top (~4.17 V+)
void  powerFake(float v); // bench: pretend the battery reads v volts (0 = real reading)   // best guess from voltage trend (no charge-status pin on this board)
