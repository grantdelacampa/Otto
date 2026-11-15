# OTTO — project context for Claude

Read this first, then `TODO.md`. `README.md` is the user-facing manual.

## What this is
OTTO is a dash companion for the owner's **1997 Mercedes-Benz E420** (W210, M119 4.2 V8): a piston
character in its cylinder that reacts to driving, greets in German, and is "unflappable". It is a
sibling of **Rotor** (`C:\Users\Grant\Documents\Rotor`, the RX-7 companion) and started as an
**independent copy** of Rotor v0.4.0's firmware (owner's choice, Nov 2026): the projects diverge
freely, so a bug fixed in one may need fixing in the other. Rotor's CLAUDE.md has the shared history
(IMU gotchas, deep sleep, voice, battery decisions) — most of it applies here too.

- **No car radio** (owner): nav, track info, radio night mode, maintenance reminders, GPS speed and
  the "standalone" dot are removed. **Bluetooth stays** for the tuning page + wireless updates only.
  `speed <mph>` survives as a generic outside speed input (nothing sends it; hook for a future
  OBD-II/GPS add-on — the E420 has OBD-II). Without it the launch timer is reaction time + peak g.
- **Board: Waveshare ESP32-S3-Touch-LCD-1.46B** (owner's, on COM4, USB 303A:1001). ESP32-S3R8,
  8 MB PSRAM, 16 MB flash. 412×412 round, **SPD2010** display (QSPI, SPI mode 3, 40 MHz here;
  Espressif's default is 20) + SPD2010 touch at **0x53 on the main I2C bus** (GPIO11/10, INT 4),
  QMI8658 0x6B, PCF85063 0x51, TCA9554 0x20 (`i2cscan` also shows an unexplained 0x7E, as on
  Rotor's board). No SHTC3. Other pins match the 1.85". Rotor's v0.3.0 (git `e4be209`) was an
  untested 1.46 port — the SPD2010 touch protocol came from there.
  - Verified on the board (Nov 14, 2026): display, touch (taps, hold/pet, crank drag), IMU, RTC,
    mic, voice models, battery ADC (~4.14 V on USB), all six designed faces.
  - SPD2010 wants window edges on multiples of 4: `display.cpp` pushes full-width 32-row stripes.
  - **Touch runs in its own task** (`readerTask` in touch.cpp, core 1, prio 2, every 10 ms) feeding
    time-stamped samples to the gesture code. The SPD2010 shows a lift only as an idle status read
    (`00 08`, no INT pulse), and the main loop is ~60-100 ms per frame, so fast taps merged into one
    long touch (petting) when it was read from the loop. `touch raw [secs]` prints every status read.
  - `perf` prints loop rate, draw / push time per frame and touch read timing. Bench (Nov 15 2026):
    voice off ~17 fps (draw 45 ms, push 12 ms at 80 MHz), "Hi ESP" ~12 fps. Glow strokes use plain
    triangles (`glowLine`): smooth wide lines on the PSRAM canvas cost ~37 ms a frame.
- **Native USB gotchas** (same as Rotor): opening the port can reset the board unless DTR/RTS are
  held low; Otto sleeps after `sleepMin` (2) minutes still and the COM port then shows "device not
  functioning" — recover with BOOT held + replug (download mode). A crash loop also blocks uploads
  the same way.
- A new board's `model` partition holds factory junk: ESP-SR crashed (LoadProhibited after
  "Please select wake words!"). `voice.cpp` now checks the srmodels header before starting.
- Tooling: `pio` is NOT on PATH — `%USERPROFILE%\.platformio\penv\Scripts\pio.exe`.

## Design
- `docs/design/Otto.dc.html` — the owner's mockup (option A): idle, disapproves ("AHEM."), launch,
  petted, sleepy, sehr gut. Palette: body silver #D6DEE8, eyes/brows ice blue #7CC8FF, pink
  #FF6AA2 + yellow #F4DC4B accents, housing #3A4452, sleepy body #8A929C, on #0D1117.
  Every other screen was derived from Rotor's in this style (owner OK'd that; replace freely).
- Fonts: the Japan Gothic font from Rotor is kept for greetings because it covers Latin-1
  (umlauts, ß). Font2/Font4/Font7 are ASCII only — keep umlauts out of captions/glosses.

## Architecture notes
- Same module layout as Rotor. `brain.*` maps each `Expr` to a `FaceState`; `face.*` draws it.
  `Expr` in `brain.h` must match `kNames[]` in `brain.cpp`; `tools/tuning.html` FACES was
  generated from kNames (regenerate it when adding one).
- **Crank model:** `FaceState.spin` is a crank speed (deg/s); main.cpp integrates it into
  `s_crankAngle`; `face.cpp` turns that into a slider-crank stroke (throw 4, rod 44, design units).
  140 deg/s = the design's 2.6 s idle pump; ~1200 = its 0.3 s launch pump. `rest` = 1 parks the
  piston at the bottom (sleep). Grab-and-spin from Rotor now turns the crank.
- Face geometry is written in the design's SVG units (viewBox 200×168) and mapped by `SX/SY`
  (cylinder) and `PP` (piston: scale, lean, stroke). Overlays are still in the 240-unit design
  space; ones that sat on Rotor's face were moved outside the cylinder walls (x 72..168).
- Otto adds `Brow`, `Mouth`, `rest`, `gray` to FaceState and `Eye::Ahem`. Rotor eye offsets
  (eyeDX/DY, eyeR) are scaled ×0.45 / ×0.75 in the renderer.
- Overlays are `uint64_t` OV_* bits — never copy `ov` into 32 bits. Radio bits were freed and
  reused: OV_AHEM (18), OV_TWINKLE (20), OV_SIDELINES (24), OV_BADGE42 (11, was 5.0).
- Settings: own NVS namespace `otto`, `SETTINGS_VERSION` restarted at 1 — bump it whenever the
  struct changes. Car show card defaults: "1997 E420" / "W210 - 4.2 V8".
- Bluetooth UUIDs end in `4f54544f0000` ("OTTO"); Rotor's end in `524f544f5200`.
- Sehr gut: main.cpp counts smooth driving seconds (reset by braking/accel/bump/dizzy/V8/rev/launch).
- Holiday swap: Rotor's Jun 23 Rotary Day became **Jan 29 PatentDay** (Benz patent 1886, the car's
  birthday) — `Holiday::PatentDay`, `Expr::HolidayPatent`, wreath text "AUTO-GEBURTSTAG".
- **OBD-II** (`obd.*`, Nov 14, 2026): BLE central to an ELM327 (the S3 has no classic BT). Pairs with
  the first adapter by name / FFF0 / 18F0 service, saves its address (NVS `obdAddr`). One command at a
  time until the '>' prompt; replies parsed by `findReply()`. Feeds `g_speed`, engine on/off in
  motion.cpp (`obdEngineState()` overrides the vibration counters), revs, MPG in trip.cpp,
  `ctx.rpm` / `ctx.coolantC`. Faces WarmUp / Warm / EngineHot / CheckEngine, OV_SHIFT (23),
  OV_THERMO (38). Off by default (`obdOn`). `obd sim on` + tools/fake_obd.py = bench without hardware.
  Never clears codes (mode 04) on purpose. Settings went to SETTINGS_VERSION 2 for the OBD keys.
- Telemetry dropped `cab`/`rh` (no SHTC3 on this board) for `obd`/`rpm`/`cool`; keep it under ~240 chars
  (BLE notify size).
- **Wi-Fi tuning** (`wifitune.*`, Nov 15 2026): SoftAP `OTTO-xxxx` (per-board password from the MAC),
  core WebServer on 192.168.4.1 serving tools/tuning.html (embedded: `board_build.embed_txtfiles`;
  delete `.pio/build/otto/tuning.html.txt.o` if a page edit doesn't show up) + /api/cmd, stat, cfg,
  log, ota. The page decides Wi-Fi vs Bluetooth by fetching /api/stat (an address check failed on iOS
  Safari). Voice pauses while it runs: with voice on only ~20 KB internal RAM is free. Wi-Fi doesn't
  give all its RAM back when stopped (voice then failed to start), so stopping does a quiet restart
  (RTC_NOINIT magic -> no boot show; RTC_DATA_ATTR does NOT survive a software restart).
- lib/ESP_SR `sr_stop()` patched: a task that failed to start has a NULL handle, and
  `vTaskDelete(NULL)` deleted the main loop (Otto froze).
- **Sleep / wake** (bench, Nov 15 2026): no touch wake on this board. The SPD2010 pulses INT on its
  own while idle (~every 0.5 s when read, ~0.16 s when not; status `00 18`), so it woke Otto ~1 s after
  every sleep. Wakes = BOOT / PWR buttons + motion peeks (tilt > ~2 deg or engine vibration; a knock
  that leaves him in place goes back to sleep, as designed). After the QMI8658's wake-on-motion the
  accel output stayed **frozen** unless soft-reset (`motionLeaveWom()`); Rotor's plain reconfigure
  wasn't enough here. Debug: `awake`, `sleeplog`, `womcheck [reset]`, `touch int`, batlog wake causes.
- Touch lift = 3 idle reads in a row in the reader task: a single idle read shows up right after
  touch-down and while sliding, which turned every swipe start into a tap.
- **Wake word stays "Hi ESP"** (owner, Nov 15 2026). "Hi Otto" was tried: Espressif has no free model
  for it, so the MultiNet command model ran always-on with "hi otto" as a command. It woke on its own
  in a quiet room (~-50 dBFS) at the same confidence as real ones (0.3-0.5) and halved the frame rate,
  even with a VAD gate; removed. A real one would be a paid custom WakeNet from Espressif.
- ESP-SR detect task runs at **priority 1** on core 1 (= loopTask) in lib/ESP_SR: at 5 it pushed the
  main loop aside. Core 0 can't take it (feed task + detect starve IDLE0 -> task watchdog reboot).
- Voice clips: German, generated with `tools/make_clips.ps1` via `tools/tts.ps1` (any WinRT voice,
  `-Lang de-DE`). **No German voice is installed on the owner's PC yet** (only en-US + ja-JP), so
  `data/clips` is empty and every clip plays the chirp for now.
