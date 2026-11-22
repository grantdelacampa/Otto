# OTTO — TODO

## Done (Nov 14, 2026)
- [x] Fork Rotor v0.4.0 firmware; port to the 1.46B (SPD2010 display + touch, 412×412)
- [x] Remove the radio (nav, track, maint, radio night, GPS speed link); BLE = tuning page + OTA
- [x] Piston character from docs/design: crank-driven stroke, brows, mouths, all six designed faces
- [x] Derive every other screen in Otto's style; German greetings + holidays; Jan 29 car's birthday
- [x] New "Sehr gut" face after 10 min of smooth driving
- [x] Voice: skip ESP-SR when the model partition is empty (new boards); srmodels flashed
- [x] Tuning page: Otto UUIDs / name / colors / faces; no radio rows
- [x] Bench: display, touch (tap, pet, crank drag), I2C devices, voice, battery ADC

## Next
- [x] Phone clock (branch `phone-clock`, Nov 22 2026): paired from iPhone Settings, clock read
- [ ] Phone clock: iOS reconnects on its own after a sleep? On-screen keyboard still shows with
      Otto (a remote) connected? The phone link doesn't stop him sleeping?
- [x] Fast taps reach grumpy again (touch reader task, Nov 15 2026)
- [x] Smoother animation: glow strokes without anti-aliasing, 80 MHz screen bus, speech task at the
      main loop's priority (voice off ~17 fps, "Hi ESP" ~12)
- [x] More frame rate (Nov 15 2026): idle with voice on ~12 -> ~26 fps (piston cache, partial
      frames, cheap cylinder). Cards / full-screen faces still draw the whole frame
- [ ] Voice: the command model mixes up "volume low" -> "volume down"; check in the car / tune
- [ ] Optional: a real "Hi Otto" wake word = a custom WakeNet model from Espressif (paid; tried an
      always-on command-model version, too many false wakes)

- [x] Sleep / wake (Nov 15 2026): touch can't wake on this board (SPD2010 INT heartbeat) -> buttons +
      motion; IMU soft reset after wake-on-motion (data was frozen, tilting never woke him)
- [x] German voice clips (Stefan) generated + on the board (Nov 15 2026)
- [x] Bench: sleep / wake, buttons, crank drag, motion faces, every derived face (Nov 15 2026)
- [ ] Rotor: check whether its sleep checks see live data too (same code; Rotor's notes say it works)

## OBD-II (software done Nov 14, 2026 and bench-tested with tools/fake_obd.py; needs the real adapter)
The 1997 E420 has OBD-II (US cars from that year: ISO 9141 K-line, slow — a few readings per second in
total; check which readings the car supports with mode 01 PID 00 once connected).
- [ ] Get a **BLE** ELM327 adapter (the ESP32-S3 has no classic Bluetooth): e.g. Vgate iCar Pro BLE,
      OBDLink CX. Prefer one with auto-sleep so it doesn't drain the car battery when parked.
- [x] `obd.*`: BLE central (scan, pair by name / FFF0 / 18F0, save the address), ELM327 setup + polling
- [x] Speed -> launch timer + trip miles; engine on/off + revs from rpm; MPG from MAF in the summary
- [x] Faces: piston synced to rpm, SHIFT ring, NOCH KALT / WARM, ENGINE HOT, CHECK ENGINE (+ menu item)
- [x] Bench: `obd sim on` + tools/fake_obd.py (scripted drive) — every face seen on the board
- [ ] With the adapter: `obd on`, check it pairs (`obd`), the protocol it finds, which PIDs the car
      supports (status line after "car answers"), and the poll rate. If an adapter needs a different
      characteristic or a PIN, that's in `bleConnect()` in obd.cpp
- [ ] In the car: is the shift rpm right for the M119, are 50 / 115 C sensible coolant limits, does the
      rpm-synced piston feel right (factor 0.2 in brain.cpp), does MPG come out plausible
- [ ] Sehr gut from throttle/speed smoothness too (now g-forces only)
- [ ] Battery drain: does the adapter sleep when the car is off?

## Battery (Nov 19 2026: 500 mAh cell, ~2 h awake)
- [x] Measure what costs what (`pwrtest`): backlight > frame rate > voice > speaker clock; CPU clock
      and BLE advertising negligible
- [x] 12 fps calm faces, speaker clock off between sounds, BLE fast 30 s then slow, WIRELESS menu switch
- [x] Charging shown when plugged into a computer at boot with a full cell
- [ ] Real fix: car USB power (lighter socket) and/or a bigger cell (1000-2000 mAh if it fits)
- [ ] Re-check awake drain in the car with `batlog` after these changes
- [ ] Optional: honest percent (correct for the sag under load) so it doesn't jump after naps
- [x] Sleep peeks every 4 s -> 15 s after 15 min parked (overnight bench: ~15 mA asleep before)
- [x] Display / touch chip put to sleep + held in reset while Otto sleeps (bench: wakes fine)
- [x] Overnight bench with 15 s peeks + display sleep: ~22 -> ~11 mV/h asleep (~10 mA left: hardware)
- [x] Auto power-off after 1 h asleep (bench-tested with `autooff 120`; PWR turns him back on)
- [x] Music bob false starts: needs a steady tempo to start (expect much less than 82 -> 44 %)
- [ ] Clock resets to 2004 when the cell goes flat -> night brightness / holidays wrong until set

## Driving fixes (Nov 19 2026, need a drive to confirm)
- [x] Rev face while cruising, drive summary at stoplights, music bob dropping out (see CLAUDE.md)
- [x] ENGINE LEARN (menu) + DRIVE LOG switch; bench: refuses on a desk (no engine), settings carried v3 -> v4
- [x] Voice "learn the road" (cruising) + status icons (mic / BT / Wi-Fi / OBD); bench: refuses when still
- [ ] In the driveway: ENGINE LEARN (idle in P, then engine off); note the idle / off mg it shows
- [ ] Early in a drive: "Hi ESP, learn the road" on a steady stretch (does the model hear the phrase?)
- [ ] Drive with the new firmware, then `drivelog`: check quiet / road / gyro at lights vs cruising,
      the M119's idle vib in Drive, and the music levels; tune QUIET_* and the engine-off level

## Possible hardware changes (owner: later, maybe)
- [ ] R42 to the GPIO0 side (3-pad footprint by the TCA9554, U11) -> a knock wakes him directly, no
      sleep peeks. Send a close-up photo first to match the pads. Firmware then has to tell BOOT and
      the knock alarm apart on GPIO0 (read the expander) and clear the expander INT before sleeping.
      `womint` checks it (GPIO0 must go low on a knock)
- [ ] Speaker amp (NS8002, U7) shutdown: lift pin 1 (SD, tied to ground) and wire it to a spare
      expander output (EXIO6) -> amp off whenever he isn't talking, asleep or awake (a few mA, always)
- [ ] Software alternative to R42: a deep-sleep wake stub that reads the IMU's knock flag in ~1 ms
      instead of a full start-up per peek (advanced: no drivers in the stub; crash loops = BOOT recovery)
