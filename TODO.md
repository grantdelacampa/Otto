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
- [ ] Optional: OBD-II speed source feeding `speed <mph>` (full 0-60 timer, trip miles)
- [ ] Optional: try LCD_QSPI_HZ 80 MHz for a faster frame push (40 MHz works)
