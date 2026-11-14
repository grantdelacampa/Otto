#include "input.h"
#include "board.h"
#include "brain.h"
#include "audio.h"
#include "motion.h"
#include "power.h"

void requestPowerOff();  // main.cpp

struct Btn {
  bool down = false, fired = false;
  uint32_t since = 0;
};
static Btn s_boot, s_pwr;

static void handle(Btn& b, bool pressed, uint32_t now, uint32_t holdMs, void (*onTap)(), void (*onHold)()) {
  if (pressed && !b.down) { b.down = true; b.fired = false; b.since = now; }
  else if (pressed && b.down && !b.fired && now - b.since >= holdMs) { b.fired = true; onHold(); }
  else if (!pressed && b.down) {
    b.down = false;
    if (!b.fired && now - b.since > 30) onTap();
  }
}

static uint32_t s_calAt = 0;   // recalibrate a moment after the hold, once the finger is off the board

static void bootTap()  { Serial.println("button: BOOT tap"); brainEvent(Expr::Boop, 1200); audioChirp(900, 1500, 120); }
static void bootHold() {
  Serial.println("button: BOOT hold -> recalibrate in 1.5 s (let go)");
  audioChirp(700, 1100, 160);       // clearly audible "got it"
  s_calAt = millis() + 1500;
}
static void pwrTap() {
  Serial.println("button: PWR tap");
  Expr e = powerCharging() ? Expr::Charging : powerPct() < 15 ? Expr::LowBattery : Expr::BatteryLevel;
  brainEventPrio(e, 2500, 45);
}
static void pwrHold()  { Serial.println("button: PWR hold -> power off"); requestPowerOff(); }

void inputUpdate(uint32_t now) {
  handle(s_boot, boardBootPressed(), now, 2000, bootTap, bootHold);
  handle(s_pwr, boardPwrPressed(), now, 3000, pwrTap, pwrHold);
  if (s_calAt && (int32_t)(now - s_calAt) >= 0 && !s_boot.down) { s_calAt = 0; motionStartCalibration(); }
}
