#pragma once
#include <Arduino.h>

// Hears music through Otto's own mic (Otto has no radio).
// The mic is already read nonstop by the "Hi ESP" listener; micRead() hands every block to musicFeed(),
// which only adds up a little energy per sample (cheap, so voice is never slowed down).
// musicUpdate() looks at the last ~6 s twice a second: loud enough, steady (not speech gaps), and a
// regular beat (autocorrelation of the bass "kicks") -> music, with a BPM estimate.
// Only works while voice is listening (that's what reads the mic).

void musicFeed(const int16_t* s, size_t n);   // any task; called from micRead()
void musicUpdate(uint32_t now);               // main loop
bool musicHeard();                            // music detected right now
float musicBpm();                             // estimated tempo (valid while musicHeard())
void musicStatus(char* b, size_t n);          // `music` command
void musicWatch(float secs);                  // `music watch [secs]`: print the numbers to serial
void musicSetThresholds(const char* what, float v);   // `music loud <dB above background>|beat|steady <v>` (bench, not saved)
void musicDebug(float& lvl, float& floor, float& beat);   // drivelog: level / background dBFS, beat strength
// Learning the car's own noise (ENGINE LEARN / learn the road): true while the analysis is running
// (MUSIC BOB on, mic being read); then the latest level (dBFS), beat strength and tempo estimate.
bool musicLive();
void musicSample(float& lvl, float& beat, float& bpm);
float musicSteady();                          // drivelog: share of the last 4 s without a gap
