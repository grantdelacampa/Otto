#pragma once
#include <Arduino.h>

// Offline voice commands (Espressif ESP-SR): wake word "Hi ESP", then one of the commands in
// voice.cpp. Needs the "model" flash partition (partitions_rotor.csv) with srmodels.bin in it:
// see README "Voice commands". Follows the VOICE setting; listens only while Otto is awake.
bool voiceInit();               // starts listening if VOICE is on and the models are there
void voiceUpdate(uint32_t now); // handles what the recogniser heard (on the main loop)
void voiceSetEnabled(bool on);  // VOICE setting changed
bool voiceRunning();
void voiceListenNow();          // skip the wake word: listen for a command now (push-to-talk / "voice listen")
void voiceStatus(char* buf, size_t n);
void voiceSetThreshold(float t); // command threshold 0..0.99 (0 = model default); not saved
