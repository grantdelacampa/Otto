#pragma once
#include <Arduino.h>

// Holiday detection and first-wake holiday screens.
// Called once per boot/wake from greetings to check whether today is special.

enum class Holiday : uint8_t {
  None,
  NewYear,       // Jan 1
  NYE,           // Dec 31
  Valentines,    // Feb 14
  July4,         // Jul 4
  Halloween,     // Oct 31
  Thanksgiving,  // 4th Thursday of November (US)
  ChristmasEve,  // Dec 24
  Christmas,     // Dec 25
  Birthday,      // matches g_set.birthdayMonth / birthdayDay
  PatentDay,     // Jan 29: Karl Benz's 1886 motorcar patent, the car's birthday
};

// Returns the holiday for today (UTC local via localHour / local date).
// Also populates out_month / out_day for display.
Holiday holidayToday(int* out_month = nullptr, int* out_day = nullptr);

// Show the holiday first-wake screen. Plays the matching greeting.
// Sets the all-day costume overlay bit in the brain.
// Returns false if greetOn is off AND it's not a birthday (we still show birthday).
bool holidayGreet();

// Should the costume overlay be on right now?
// (party hat, Santa hat, or jack-o-lantern for the rest of the day)
bool holidayCostumeOn();

// Which holiday's costume is on today (Holiday::None if no costume).
Holiday holidayCostumeType();

// Called by main on every new local date to clear the "greeted today" flag.
void holidayDayTick();
