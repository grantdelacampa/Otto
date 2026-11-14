#pragma once
#include <Arduino.h>

// OBD-II through a Bluetooth LE ELM327 adapter (Otto is the BLE central; the tuning page link is
// unaffected). The 1997 E420 talks ISO 9141 (slow K-line): speed + rpm are polled continuously,
// coolant / throttle / MAF / lamp status in turn.
//
// Bench: `obd sim on` sends the adapter's requests over USB serial ("obd> 010C") and takes replies
// from tools/fake_obd.py ("obd< 41 0C 1A F8"), so everything above the radio link can be tested
// without a car.

struct ObdState {
  bool  adapter = false;      // link to the ELM327 is up (or the bench sim is on)
  bool  ecu = false;          // the car answers (protocol found)
  char  name[24] = "";        // adapter name (BLE) or "SIM"
  char  proto[32] = "";       // protocol the ELM327 found (ATDP)
  float rpm = -1;             // -1 = unknown
  float kmh = -1;
  float coolantC = -100;      // -100 = unknown
  float throttle = -1;        // %
  float maf = -1;             // g/s (only if the car supports PID 10)
  uint32_t rpmAt = 0, speedAt = 0, coolAt = 0;
  bool  mil = false;          // check-engine lamp on
  uint8_t nDtc = 0;           // stored codes the car reports
  uint8_t nCodes = 0;         // codes read so far (mode 03)
  char  codes[8][6] = {};     // "P0171"
  uint32_t pids[2] = {0, 0};  // supported mode-01 PIDs 01-20, 21-40 (bit 31 = lowest PID)
};
extern ObdState g_obd;

void  obdInit();
void  obdUpdate(uint32_t now);
void  obdDisconnect();                 // before deep sleep
int   obdEngineState();                // -1 unknown (no adapter / car not answering yet), 0 off, 1 running
bool  obdRpmFresh();                   // rpm read in the last 2 s
float obdRpm();                        // 0 if not fresh
float obdFuelGps();                    // fuel burned, g/s (from MAF), -1 if unknown
void  obdShowCodes();                  // check-engine card(s), or "no codes"
void  obdCommand(const char* args, char* reply, size_t n);   // the "obd ..." console command
void  obdSimRx(const char* text);      // "obd< ..." reply lines from tools/fake_obd.py
