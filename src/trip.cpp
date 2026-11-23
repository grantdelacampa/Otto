#include "trip.h"
#include "drivelog.h"
#include "brain.h"
#include "motion.h"
#include "commands.h"
#include "extras.h"
#include "obd.h"

struct Trip {
  bool active = false;
  uint32_t start = 0, lastDrive = 0;
  float miles = 0, maxLat = 0, maxBrake = 0;
  float fuelG = 0, fuelMiles = 0;           // OBD MAF: fuel burned, and the miles it covered
  uint16_t brakes = 0, accels = 0, turns = 0, bumps = 0;
};
static Trip s;

void tripReset() { s = Trip(); }
uint32_t tripStartMs() { return s.active ? s.start : 0; }

void tripHard(Hard h) {
  switch (h) {
    case Hard::Brake: s.brakes++; break;
    case Hard::Accel: s.accels++; break;
    case Hard::Turn:  s.turns++; break;
    case Hard::Bump:  s.bumps++; break;
  }
}

void tripUpdate(uint32_t now, float dt) {
  bool driving = g_motion.ok && g_motion.stoppedSecs < 1.0f;
  if (driving) {
    if (!s.active) { s.active = true; s.start = now; }
    s.lastDrive = now;
  }
  if (!s.active) return;
  bool spd = g_speed.at && (int32_t)(now - g_speed.at) < 2000;
  if (spd) s.miles += g_speed.mph * dt / 3600.0f;
  float fuel = obdFuelGps();
  if (fuel >= 0 && spd) { s.fuelG += fuel * dt; s.fuelMiles += g_speed.mph * dt / 3600.0f; }
  s.maxLat = fmaxf(s.maxLat, fabsf(g_motion.lat));
  s.maxBrake = fmaxf(s.maxBrake, -g_motion.lon);
}

static uint32_t driveSecs() { return s.active ? (s.lastDrive - s.start) / 1000 : 0; }

bool tripReady() { return s.active && driveSecs() >= 180; }

bool tripShowSummary() {
  if (!tripReady()) return false;
  uint32_t secs = driveSecs();
  float hours = fmaxf(secs / 3600.0f, 0.1f);
  float weighted = s.brakes * 2.0f + s.accels + s.turns + s.bumps * 0.5f;
  int score = (int)constrain(100.0f - (weighted / hours) * 1.2f, 30.0f, 100.0f);

  char label[24], big[8], sub[28];
  snprintf(label, sizeof(label), "DRIVE - %u MIN", (unsigned)((secs + 30) / 60));
  snprintf(big, sizeof(big), "%d", score);
  float gallons = s.fuelG / 2790.0f;        // ~2.79 kg of petrol per US gallon
  if (s.fuelMiles >= 1.0f && gallons > 0.01f) snprintf(sub, sizeof(sub), "%.1f MI - %.0f MPG", s.miles, s.fuelMiles / gallons);
  else if (s.miles >= 0.1f) snprintf(sub, sizeof(sub), "%.1f MI - %.1fG TURN", s.miles, s.maxLat);
  else snprintf(sub, sizeof(sub), "%.1fG TURN - %.1fG BRAKE", s.maxLat, s.maxBrake);
  brainSetAnswer(label, big, sub);
  brainEvent(Expr::Summary, 7000);
  drivelogEvent("summary");
  tripReset();
  extrasDriveDone();                     // drive count + milestones
  return true;
}
