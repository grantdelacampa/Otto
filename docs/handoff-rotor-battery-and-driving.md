# Handoff: port Otto's battery + driving fixes to Rotor

**For:** an agent working in Rotor's repo (`C:\Users\Grant\Documents\Rotor`).
**From:** Otto (`C:\Users\Grant\Documents\otto`), the sibling project. Otto started as a copy of
Rotor v0.4.0 firmware, so most of the code paths are the same. **Read Otto's code for each fix
before porting it**: the commits are `297e91f` (the main work) and `d699bb5` (peek-count logging) on
Otto's `bench` branch. Otto's `CLAUDE.md` has the details of each change and why.

**Goal:** bring over the fixes that apply to Rotor, check each one against Rotor's hardware (Waveshare
ESP32-S3-Touch-LCD-1.85, not Otto's 1.46B), and skip the ones that don't fit. Don't copy blindly:
Rotor differs in the display / touch controller, the amp chip, the radio link, and its settings layout.

## Why this happened (Otto, Nov 19-20 2026)

Otto (500 mAh cell) died after ~2 h awake and lost ~38 % overnight asleep. Measured:

- **Awake ~270 mA.** An on-battery A/B test (`pwrtest`, see below) ranked the costs. The battery
  sags in proportion to current, so each change was compared against baseline steps on either side:
  backlight ~half of what can be saved; frame rate ~1/4; "Hi ESP" voice ~1/6; the speaker's I2S
  clock a little. **CPU 240 -> 160/80 MHz and BLE advertising: too small to measure** (CPU left at 240).
- **Asleep ~15 mA.** Two causes: (1) the 4 s motion peek is a full chip start-up (incl. the PSRAM
  self-test) every 4 s; (2) at sleep the display chip only got DISPOFF, so it kept running (its touch
  half kept scanning: the INT heartbeat seen during sleep).

Rotor has a 2000 mAh cell, so this is less urgent, but the same code costs the same current.

## Fixes, in suggested order

Each item gives what Otto did, where it is, and what to check on Rotor.

### 1. Measurement tools first (so every change can be checked)
- **`batlog`** already exists in Rotor (power.cpp). Otto added: a `peeks<N>` line after each wake
  from sleep (main.cpp, right after `powerLog(why)` in setup), showing how many motion peeks the
  sleep took. Port it.
- **`pwrtest`** (new `src/pwrtest.*`): `pwrtest start [secs]` then unplug USB. It steps through
  settings (backlight, fps, CPU MHz, voice, BLE advertising, speaker clock) for ~20 s each, averages
  the battery ADC, writes `/pwrtest.csv`, then restarts. `pwrtest` prints the results as mV of
  sag saved against the neighbouring baselines.
  Gotchas found on Otto: on USB the ADC reads the charger, and `powerCharging()` can miss a plug-in at
  boot with a near-full cell. So the test waits for `!Serial.isPlugged()` (native USB SOF frames)
  plus 15 s. Hold the serial port open from a background script until the unplug: closing it can
  reset the board.
- **`drivelog`** (new `src/drivelog.*`): one CSV line every 2 s while not still (vib, road, gyro
  wobble, stoppedSecs, quietSecs, engine, face, music level / background / beat / bob), plus event
  markers. Buffered and written every 30 s; menu toggle DRIVE LOG. Useful for tuning #6 from a real drive.

### 2. Frame rate for calm faces (main.cpp `frame()`)
Idle / Bored / Sleep faces (no menu, no touch, no crank drag, spin < 200 deg/s) draw at 12 fps
(`IDLE_FRAME_MS = 83`), everything else at 30. Otto's real rate at "30 fps" was ~19 (frames took
~50 ms). **Rotor:** check that its idle face reads fine at 12 fps (the rotor spins rather than
pumping, so maybe pick ~15). Partial-frame rendering, if Rotor has it, matters too.

### 3. Speaker I2S clock off between sounds (audio.cpp)
`i2s_channel_disable(s_i2s.txChan())` after each sound (when the queue is empty) and enable before the
next one. Enable **before** `configureTX()`, which disables and re-enables internally. With the clock
stopped the PCM5101 goes to standby by itself. Owner heard no pops on Otto. **Rotor:** same PCM5101,
but its amp is an **APA2068**: check Rotor's schematic for an amp enable / shutdown pin. On Otto the
NS8002's SD pin is tied to ground (always on, a few mA, even asleep), and that's not fixable in firmware.

### 4. Voice naps + mic reader (voice.cpp)
"Hi ESP" listens for 5 min after wake / engine start / touch / menu / any use, then `ESP_SR.end()`.
**Important:** the music bob was fed only by ESP-SR's mic reads (`micRead()` -> `musicFeed()`), so
napping killed the bob. Otto added a `micbob` task (core 1, prio 1) that reads the mic while ESP-SR
is off. `micRead` isn't reentrant: `start()` raises `s_srStarting` and waits for the task's block.
A small mic icon shows while listening. Side effect: drawing got faster while voice naps
(35 -> 23 ms per frame: less PSRAM contention). Bench command `listen nap`.
**Rotor: ask the owner before porting.** Rotor's voice also drives the radio ("radio volume up",
CarPlay Siri ring), so a nap may not be wanted, or should be gated on the radio link.

### 5. Sleep: display chip asleep + slower peeks (main.cpp, display.cpp, board.cpp)
- `displayOff()`: after DISPOFF, send **SLPIN (0x10)** and wait 120 ms. Otto's SPD2010 driver had no
  `disp_sleep`, so it's sent as `esp_lcd_panel_io_tx_param(io, (0x02<<24)|(0x10<<8), NULL, 0)`.
- `enterDeepSleep()`: hold TP_RST + LCD_RST low via the TCA9554. `boardEarlyInit()` keeps them low
  after a deep-sleep wake (`esp_reset_reason() == ESP_RST_DEEPSLEEP` -> expander 0xFC), so a peek
  doesn't start the display chip; `displayInit()` / `touchInit()` pulse their own resets on a full boot.
- Peek interval: 4 s for the first 15 min parked (`s_peeks < 225`), then 15 s (`peekSecs()`). The
  IMU's wake-on-motion flag latches between peeks, so a door opening still counts.
- **Rotor:** a different display controller (1.85"). Check its driver / datasheet for SLPIN and how
  its reset lines are wired (Otto's notes say the 1.85 pins match the 1.46B except the display:
  verify on Rotor's schematic). Verify a BOOT wake brings the display + touch back. Otto's
  overnight result with these is still pending.
- Hardware note: on the 1.46B, the TCA9554 INT reaches GPIO0 only if R42 is moved (bench test
  `womint`: the IMU INT toggled at the expander but GPIO0 never went low). The 1.85 may have the
  same footprint: check Rotor's schematic before suggesting a mod.

### 6. Driving logic: "really stopped" vs cruising (motion.cpp) - probably the most valuable
Otto's drive showed three bugs that come from `stoppedSecs` counting up while **cruising straight**
(no accel / brake / turn): road shake made **Rev faces**, a smooth idle at a light read as **engine
off -> drive summary at every stop**, and the music bob dropped out. Rotor has the same `stoppedSecs`
logic. Fixes:
- `quietSecs`: **gyro wobble** (|gyro - its own ~5 s average|, seeded from the first sample)
  smoothed 1 s < `quietGyro` (0.6 dps), **and** vertical 0.5-3 Hz road RMS < `quietRoad` (12 mg).
  **Use wobble, not raw gyro:** Otto's raw gyro carried a 7.6 dps offset whenever the offset couldn't
  re-learn. Rotor's notes mention ~4 dps, so the same trap applies.
- Revs need `quietSecs > 3` before the jump. Engine off needs vib < `engOffVib` (or 0.3 x engineVib)
  with `quietSecs > 1` for 8 s. The summary waits 20 s and needs the engine still off and stopped.
- Music bob hysteresis: once bobbing, looser limits; it takes 8 s (not 3) of no music to stop.
- **Rotor-specific:** when the radio link supplies speed, prefer it (speed > ~2 mph = not stopped).
  Keep the IMU logic for standalone mode.

### 7. Per-car learning (after #6): ENGINE LEARN recommended, learn the road optional
Owner, Nov 20 2026: **port ENGINE LEARN for Rotor.** A rotary idles very smoothly, so idle vs engine-off
by vibration is harder than on Otto's V8, and Rotor's in-car idle vibration test was never done (its
engine thresholds are untested defaults). "Learn the road" only matters in standalone mode (no radio
speed). Port it only if the owner wants it.
- **ENGINE LEARN** (menu / `enginelearn` / voice "learn the engine"): 20 s idling in P, then engine
  off 10 s. Sets idleVib, engineVib = sqrt(off x idle), engOffVib = sqrt(off x on), idleGyro / Road.
  Refuses if idle < 2.5 x off.
- **Learn the road** (voice "learn the road" / "learn driving", hand-encoded phonemes `LkN jc RbD`,
  `LkN DRiVgl`; `enginelearn road`): 20 s cruising; the 20th percentile of wobble / road ->
  cruiseGyro / Road. `recomputeQuiet()` puts the quiet limits at sqrt(idle x cruise), but only once
  ENGINE LEARN has measured idle.
- **Rotor:** with radio speed available these matter less, but they help standalone mode.
  Rotor's voice phrases are Japanese-greeting / English-command; check ESP-SR command count limits.

### 8. Small things
- BLE: Otto advertises fast for 30 s after wake / disconnect / menu, then slow (advertising cost
  nothing measurable), and has a WIRELESS off switch. **Rotor: probably skip.** The radio link needs
  Bluetooth.
- Status icons (`faceSetStatus`, a lower-left 2x2: mic / BT connected / Wi-Fi / OBD). Drawn only on
  full frames, plus one full frame when they change. Optional.

## Pitfalls hit on Otto (save yourself the time)

- **Settings migration:** don't bump `SETTINGS_VERSION` in a way that resets settings: Rotor's notes
  say a bump wipes calibration. Otto appends new fields at the **end** of the struct and loads an
  older blob as a prefix. Mind the **tail padding** when the old struct ended on a bool: copy only
  `offsetof(lastField) + sizeof(bool)` bytes. See Otto's `settingsLoad()` (V2_SIZE / V3_USED / V4_SIZE).
- **Bench orientation:** on the bench the board reads its lean as constant braking (Rotor's notes say
  the same). The gyro offset only re-learns when |lat| < 0.05 g, so stillness / quiet checks can fail
  on a desk. Test the stillness logic with the board in its mounted orientation.
- **Test timing with the owner:** the owner reads messages a few seconds late. Give long windows (45 s+)
  for "knock now" style tests and say it before starting the command.
- **Sleep tests:** don't start one straight off the charger. A fresh cell settles downward for 1-2 h
  and looks like drain. Unplug, keep it awake 15-20 min, then sleep 3 h+ / overnight.
- Native USB: opening / closing the port can reset the board; a sleeping board has no COM port
  (wake it with BOOT).

## Done means
Each ported fix is built, flashed and checked on Rotor's board (bench), documented in Rotor's
`CLAUDE.md` / README / TODO, and committed on a branch. Fixes that don't apply are written down with
the reason. Ask the owner before #4 (voice naps) and before any hardware suggestion.
