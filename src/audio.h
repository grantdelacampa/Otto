#pragma once
#include <Arduino.h>

// Onboard speaker (PCM5101 DAC). Plays 16-bit WAV clips from LittleFS
// (/clips/<name>.wav) and simple synthesized chirps, on a background task.

bool audioInit();
void audioPlayClip(const char* name);               // falls back to a chirp if the file is missing
void audioChirp(float f0, float f1, uint16_t ms);
bool audioBusy();
void audioHoldClock(bool on);   // pwrtest: keep the speaker clock running between sounds (the old way)
