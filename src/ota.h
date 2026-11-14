#pragma once
#include <Arduino.h>

// Firmware updates over Bluetooth. The tuning page (or radio) sends:
//   "ota begin <size>"  on CMD, waits for {"msg":"ota ready"}
//   the .bin in chunks   on the OTA characteristic (write with response)
//   "ota end"            on CMD -> verifies, replies "ota ok", reboots
// These run directly in the Bluetooth task so data never races the begin.
bool  otaBegin(size_t size);
bool  otaWrite(const uint8_t* data, size_t len);
bool  otaEnd();
void  otaAbort();
bool  otaActive();
float otaProgress();
void  otaUpdate(uint32_t now);   // main loop: progress screen, timeout, reboot
const char* otaError();
