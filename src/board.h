#pragma once
#include <Arduino.h>
#include <time.h>

// Low-level board bits: I2C, IO expander, power latch, battery, buttons, RTC.

void boardEarlyInit();                    // power latch + I2C + expander (call first)
bool boardI2CRead(uint8_t addr, uint8_t reg, uint8_t* buf, size_t n);
bool boardI2CWrite(uint8_t addr, uint8_t reg, uint8_t val);
void boardExpanderWrite(uint8_t bit, bool high);

float boardBatteryVolts();                // filtered by caller
void  boardPowerOff();                    // releases the power latch (only turns off on battery)
void  boardHoldPinsForSleep();
void  boardReleasePinHolds();

// Buttons (active LOW)
bool boardBootPressed();
bool boardPwrPressed();

// RTC (PCF85063) keeps UTC. Local time uses the stored tz offset.
bool   rtcInit();                         // returns true if the RTC holds a valid time
bool   rtcValid();

// SHTC3 cabin temperature / humidity (main I2C bus, 0x70). NOT fitted on the owner's 1.85" board
// (i2cscan: no 0x70); the driver detects that and stays off, so it only matters on boards that have one. Measured every 5 s without blocking:
// call shtc3Update() from the main loop. Values are from the last good reading.
bool   shtc3Init();                           // false if the sensor isn't there
void   shtc3Update(uint32_t now);
bool   shtc3Ok();                             // a reading has succeeded
float  shtc3TempC();
float  shtc3Humidity();                       // % RH
time_t rtcNowUtc();                       // 0 if invalid
void   rtcSetUtc(time_t t);
int    localHour();                       // -1 if time unknown
void   setTzOffsetMin(int m);
int    tzOffsetMin();
