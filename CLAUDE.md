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
- Voice clips: German, generated with `tools/make_clips.ps1` via `tools/tts.ps1` (any WinRT voice,
  `-Lang de-DE`). **No German voice is installed on the owner's PC yet** (only en-US + ja-JP), so
  `data/clips` is empty and every clip plays the chirp for now.
