#include "ota.h"
#include "brain.h"
#include <Update.h>

static volatile bool s_active = false;
static volatile size_t s_size = 0, s_done = 0;
static volatile uint32_t s_lastData = 0, s_rebootAt = 0;
static char s_err[48] = "";

bool otaActive() { return s_active; }
float otaProgress() { return s_size ? (float)s_done / s_size : 0; }
const char* otaError() { return s_err; }

bool otaBegin(size_t size) {
  if (s_active) Update.abort();
  s_err[0] = 0;
  if (size < 100000 || !Update.begin(size, U_FLASH)) {
    snprintf(s_err, sizeof(s_err), "begin failed (%u)", (unsigned)Update.getError());
    s_active = false;
    return false;
  }
  s_size = size;
  s_done = 0;
  s_lastData = millis();
  s_active = true;
  return true;
}

bool otaWrite(const uint8_t* data, size_t len) {
  if (!s_active) return false;
  if (Update.write((uint8_t*)data, len) != len) {
    snprintf(s_err, sizeof(s_err), "write failed (%u)", (unsigned)Update.getError());
    Update.abort();
    s_active = false;
    return false;
  }
  s_done += len;
  s_lastData = millis();
  return true;
}

bool otaEnd() {
  if (!s_active) return false;
  s_active = false;
  if (s_done != s_size || !Update.end(true)) {   // end() also checks the image's checksum
    snprintf(s_err, sizeof(s_err), "verify failed (%u/%u)", (unsigned)s_done, (unsigned)s_size);
    return false;
  }
  s_rebootAt = millis() + 1500;
  return true;
}

void otaAbort() {
  if (s_active) Update.abort();
  s_active = false;
}

void otaUpdate(uint32_t now) {
  if (s_active) {
    brainSetRing(otaProgress());
    brainEventPrio(Expr::Updating, 600, 110);
    if (now - s_lastData > 20000) { otaAbort(); snprintf(s_err, sizeof(s_err), "timed out"); brainEvent(Expr::Confused, 2000); }
  }
  if (s_rebootAt && (int32_t)(now - s_rebootAt) >= 0) ESP.restart();
}
