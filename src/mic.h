#pragma once
#include <Arduino.h>

// ICS-43434 I2S MEMS microphone (TODO "Voice commands", bring-up step).
bool micInit();
bool micOk();
// Reads up to `frames` mono samples (16 kHz, 16-bit, already gain-scaled). Returns frames read.
size_t micRead(int16_t* out, size_t frames);

// `mic [secs]` bench test: prints the sound level every 100 ms (dBFS RMS + peak + a bar),
// so we can see whether it hears a voice clearly over background noise.
void micLevelTest(float secs, uint16_t src, void (*reply)(uint16_t, const char*));
