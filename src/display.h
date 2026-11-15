#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "config.h"

// All drawing happens in an LCD_W x LCD_H (360x360) sprite in PSRAM (LovyanGFX is
// used only as a drawing library). Each frame is pushed to the SPD2010 panel over
// QSPI with Espressif's esp_lcd_spd2010 driver (bundled in lib/).
extern LGFX_Sprite canvas;

bool displayInit();
void displayPush();
void displayPushRect(int x, int y, int w, int h);   // part of the canvas (edges rounded to multiples of 4)
void displayBacklight(uint8_t level);  // 0..255 target, eased
void displayUpdateBacklight(float dt);
void displayOff();                      // backlight off + panel off (before deep sleep)
