#pragma once
#include <Arduino.h>

// On-device config menu (tap, then tap-and-hold; only when the car is stopped).
bool menuIsOpen();
void menuOpen();
void menuClose(bool save);
void menuTouched(uint32_t now);           // any touch resets the idle timer
void menuTap(float x, float y);           // panel pixels
void menuSwipe(float dyDesign);           // + = down, in 240-design units
void menuUpdate(uint32_t now);            // auto-close on idle / movement
void menuRender(uint32_t now);            // draws the whole screen into the canvas
