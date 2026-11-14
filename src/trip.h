#pragma once
#include <Arduino.h>

// Per-drive stats for the end-of-drive summary card.
enum class Hard : uint8_t { Brake, Accel, Turn, Bump };

void tripUpdate(uint32_t now, float dt);
void tripHard(Hard h);                 // motion.cpp: a hard event just started
bool tripReady();                      // at least 3 minutes of driving recorded
bool tripShowSummary();                // builds + shows the card; returns false if not ready
void tripReset();
uint32_t tripStartMs();                // millis() when this drive started, 0 = no drive
