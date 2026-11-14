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
- [ ] **Fast taps don't reach grumpy (5 taps in 2 s)** — quick taps on one spot merge into one long
      touch, which turns into petting. Found so far (`touch raw [secs]` prints every SPD2010 status):
      a finger reads `01 18 len 10`, and a lift is simply the next read showing idle `00 08 len 0`
      (no INT pulse); holds of 3-5 s never show `00 08`. readFrame() now returns 0 (lifted) on that,
      but it still didn't help — next suspect is how often touchUpdate() actually reads: in the raw
      log each read took ~60 ms even with a 15 ms delay, so with the frame render in between a short
      lift between taps can fall between two reads. Measure readFrame() time, speed it up (the drain
      loop / 200 us delays) and/or poll touch more often (own task, or between display stripes).
      Serial now logs `touch: tap (N in 2 s)` and `touch: not a tap - held N ms` to check it.
- [ ] Voice: "Hi Otto" works (bench: sure 0.50-0.52, threshold 0.30). The command model mixes up
      "volume low" -> "volume down" and missed "what time is it" once; check in the car / tune.
- [ ] Add a German Windows voice, run `tools/make_clips.ps1`, then `pio run -t uploadfs`
- [ ] Check touch direction: drag clockwise around the edge -> serial `let go at +N deg/s` and the
      crank turns with the finger. If it's reversed or menu swipes feel upside down, set the
      TOUCH_* flags in config.h
- [ ] Battery: confirm PWR hold (GPIO7), PWR key (GPIO6) and battery ADC (GPIO8) on the 1.46B with
      a cell fitted (`batlog`, power off by holding PWR 3 s). Check battery plug polarity first.
- [ ] Speaker: fit one to the speaker header and check `play` / chirps (no clips yet)
- [ ] In the car: calibrate, brake/launch/turn faces, engine-on vibration level (`engineVib` on the
      CAR tab — the M119 will idle differently from Rotor's 5.0), Sehr gut after a calm drive
- [ ] Look at every derived screen on the board (`expr <name>`, tuning page face tester) and redesign
      any that don't fit Otto
- [ ] Mount / enclosure for the 1.46B (Ø ~1.46" active area; Waveshare has a structural drawing in
      Downloads/ESP32-S3-Touch-LCD-1.46_Structural Diagram-With Cover)
- [ ] Optional: try LCD_QSPI_HZ 80 MHz for a faster frame push (40 MHz works)

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
