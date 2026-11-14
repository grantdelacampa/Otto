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
- [x] Fast taps reach grumpy again (touch reader task, Nov 15 2026)
- [x] Smoother animation: glow strokes without anti-aliasing, 80 MHz screen bus, speech task at the
      main loop's priority (voice off ~17 fps, "Hi ESP" ~12)
- [ ] More frame rate: the draw is still ~45 ms (PSRAM canvas); ideas: only redraw the changing
      area, cache the static cylinder, fewer smooth strokes
- [ ] Voice: the command model mixes up "volume low" -> "volume down"; check in the car / tune
- [ ] Optional: a real "Hi Otto" wake word = a custom WakeNet model from Espressif (paid; tried an
      always-on command-model version, too many false wakes)

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
