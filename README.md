# OTTO — dash companion firmware

Otto is a dash companion for a **1997 Mercedes-Benz E420** (W210, 4.2 V8): a piston in its
cylinder that pumps slowly at idle, strokes hard on launches, raises one eyebrow at hard braking
("AHEM."), and greets you in German. Rotor spins, Otto strokes.

It's a sibling of [Rotor](https://github.com/grantdelacampa/Rotary): the same motion, touch,
voice, holiday and extras logic, on a different board, with its own character and **no car
radio** (fully standalone; Bluetooth is only for the tuning page and wireless updates).

Board: **Waveshare ESP32-S3-Touch-LCD-1.46B** — 1.46" 412×412 round IPS with an SPD2010
display + touch controller (QSPI / I2C), QMI8658 gyro, PCF85063 clock, PCM5101 speaker DAC,
I2S mic, ESP32-S3R8 (8 MB PSRAM, 16 MB flash).

Design reference: [docs/design/Otto.dc.html](docs/design/Otto.dc.html) (open it through a local
web server, see [docs/design/README.md](docs/design/README.md)).

---

## 1. Setup

1. VS Code + the **PlatformIO** extension (or PlatformIO Core:
   `%USERPROFILE%\.platformio\penv\Scripts\pio.exe` if `pio` isn't on PATH).
2. Open this folder and plug the board in with USB-C.

The SPD2010 display driver is bundled in `lib/esp_lcd_spd2010` and touch is read directly in
`src/touch.cpp`, so there's nothing extra to download.

## 2. Build and flash

```
pio run -t upload          # firmware over USB
pio run -t uploadfs        # voice clips in data/clips (see data/clips/README.txt)
pio device monitor         # serial console, 115200
```

**Speech models (voice commands, once per board):**

```
%USERPROFILE%\.platformio\penv\Scripts\python.exe %USERPROFILE%\.platformio\packages\tool-esptoolpy\esptool.py ^
  --chip esp32s3 --port COM4 --baud 921600 write_flash 0xD10000 ^
  %USERPROFILE%\.platformio\packages\framework-arduinoespressif32-libs\esp32s3\esp_sr\srmodels.bin
```

Without them Otto works normally, just without voice (`listen` says `models MISSING`). A new
board's `model` partition holds leftover factory data; the firmware checks the model header and
skips voice instead of crashing.

**If the upload won't connect** (port busy / "device not functioning"): Otto may be asleep, or
the native USB is wedged after a sleep. Hold **BOOT**, unplug and replug USB-C, release BOOT,
upload again. The screen stays dark in download mode; that's expected.

**Wireless updates:** `pio run`, then on the tuning page open **UPDATE** and pick
`.pio/build/otto/firmware.bin`.

## 3. First boot

The piston draws itself, then **OTTO // E420 // 4.2 V8**, then a German greeting. Calibration
starts the first time: mount it, keep the car still for 2 seconds ("HOLD STILL" ring). If
braking shows the launch face, type `flipfwd`.

## 4. Faces

Designed expressions (docs/design) and what triggers them:

| Design | When | Firmware name |
|---|---|---|
| IDLE | nothing going on; slow pump, blinks, glances | `idle` |
| DISAPPROVES — "AHEM." | hard braking (g shown below) | `braking` |
| LAUNCH | hard acceleration, V8 launches, revs | `accel`, `v8`, `rev` |
| PETTED | hold a finger still on the screen | `pet` |
| SLEEPY | late night / going to sleep: gray, resting at the bottom of the stroke | `sleepy`, `sleep` |
| SEHR GUT | 10 minutes of smooth driving (no hard braking, launches or bumps) | `sehrgut` |

Every other Rotor screen (turns, bumps, dizzy, battery, holidays, drag tree, guard, cabin
temperature, car show, …) is redrawn in Otto's style. Try any with `expr <name> [ms]`, or the
face tester on the tuning page.

Greetings: Guten Morgen! · Guten Tag! · Guten Abend! · Willkommen! · Hallo! · Los geht's! ·
Bereit. · Müde… (late night), with the English meaning underneath.

Holidays: birthday (Alles Gute!), New Year, NYE (Guten Rutsch!), Valentine's, **Jan 29: the
car's birthday** (Benz's 1886 patent — Otto's version of Rotor's Rotary Day), July 4th,
Halloween (jack-o'-lantern piston), Thanksgiving, Christmas.

## 5. Touch and buttons

| Gesture | What happens |
|---|---|
| Tap | Random reaction: happy sparkle, wobble or startled. 5+ taps in 2 s gets grumpy |
| Hold one finger 0.8 s | Petting, for as long as you hold |
| Drag around the edge / flick | Turn the crank: the piston strokes with your finger, coasts when you let go; a hard flick ends dizzy |
| Tap, then tap and hold 0.6 s | Config menu (only when stopped) |

Menu: BRIGHTNESS, NIGHT BRIGHT, VOLUME, GREETINGS, REVS, V8 MODE, MUSIC BOB, BORED, YAWNS,
HOLIDAYS, MOTION WAKE, BIRTHDAY, VOICE, PRO TREE, LAUNCH MODE, CAR SHOW, GUARD, RECALIBRATE,
INFO, EXIT. Swipe up/down to scroll, tap to pick; it closes after 15 s untouched or when you
drive off.

| Button | Tap | Hold |
|---|---|---|
| BOOT | Boop | 2 s: recalibrate mount |
| PWR  | Battery level | 3 s: power off (on battery) |

If touches land in the wrong place, type `touch`, tap the corners, and set
`TOUCH_SWAP_XY` / `TOUCH_MIRROR_X` / `TOUCH_MIRROR_Y` in `src/config.h`.

## 6. Tuning page

`tools/tuning.html`, over Web Bluetooth (Chrome / Edge). Browsers only allow Bluetooth on
localhost or https:

```
cd tools
python -m http.server 8000
```

Open http://localhost:8000/tuning.html and press CONNECT (device name `OTTO`). It uses its own
Bluetooth service UUIDs (`…-4f54544f0000`), so it never connects to Rotor and Rotor's page
never connects to Otto.

## 7. Serial console

Same commands over USB and Bluetooth. `help` lists them:

| Command | What it does |
|---|---|
| `stat` | live telemetry JSON |
| `obd ...` | OBD-II adapter (section 8) |
| `get` / `cfg {"brake":0.4}` | read / change settings (partial JSON) |
| `expr braking 3000` | show any expression for N ms |
| `calibrate`, `flipfwd` | mount calibration / flip forward axis |
| `greet`, `boop` | a greeting / boop |
| `log`, `summary` | drive log as CSV / drive summary now (3+ min of driving) |
| `launch arm`, `launch resetbest` | arm the drag tree / clear best times |
| `speed [mph]` | outside speed input (nothing sends it on Otto; `tools/fake_gps.py` on the bench). With it the launch timer also does 0-30 / 0-60 |
| `touch` | raw touch points for 5 s |
| `time <unix> <tz-minutes>`, `clock` | set / show the clock (the tuning page sets it on connect) |
| `vibcap`, `mic`, `music`, `listen` | idle vibration test, mic level, music detection, voice status |
| `i2cscan` | devices on the I2C bus (expect 0x20, 0x51, 0x53, 0x6B) |
| `batlog`, `batfake <V>\|off` | battery log / pretend battery voltage |
| `show on\|off`, `extras [guard\|hot\|cold\|missed\|milestone\|break\|patent]` | car show mode / extras demos |
| `play <clip>` | play a voice clip, e.g. `play morgen` |
| `holidays [on\|off]`, `birthday M D` / `birthday clear` | holidays + birthday |
| `wom`, `imulp`, `sleep`, `reset` | debug / sleep now / reset settings (keeps calibration) |

## 8. OBD-II (optional adapter)

Otto can read the car through a **Bluetooth LE ELM327 adapter** in the E420's OBD-II port (it has to
be BLE: the ESP32-S3 has no classic Bluetooth — e.g. Vgate iCar Pro BLE, OBDLink CX). Turn it on
with the menu's **OBD ADAPTER** row, the tuning page, or `obd on`. Otto scans every 20 s, pairs with
the first adapter it finds (by name or its FFF0 / 18F0 service) and then only uses that one
(`obd forget` to pair a different one). The 1997 E420 talks ISO 9141, which is slow: a few readings
per second in total.

With it:
- **Speed** feeds the launch timer (0-30 / 0-60, timeslips, NEUER REKORD) and trip miles.
- **Engine on/off** comes from rpm instead of guessing from vibration; **revs** from real rpm.
- The **piston pumps with the engine** (700 rpm idle = the design's slow pump, 6000 = flat out).
- **SHIFT** ring flashes at `shiftRpm` (default 5800, 0 = off).
- **NOCH KALT** warm-up face after a cold start (coolant below `coldCoolC`, 50 °C), then **WARM**;
  **ENGINE HOT** if coolant reaches `hotCoolC` (115 °C), repeated every minute.
- **CHECK ENGINE** card with the stored codes (e.g. P0171), ~11 s after the engine starts, or any
  time from the menu's **ENGINE CODES** / `obd codes`. Otto only reads codes; it never clears them.
- **MPG** in the drive summary, if the car reports air flow (PID 10).

| Command | What it does |
|---|---|
| `obd` | status: adapter, protocol, rpm, speed, coolant, lamp, codes |
| `obd on\|off` | use the adapter or not (saved) |
| `obd forget` | forget the paired adapter |
| `obd codes` | show the check-engine card |
| `obd raw <cmd>` | send any ELM327 command (e.g. `obd raw 0105`) and print the reply on serial |
| `obd sim on\|off` | bench: talk to `tools/fake_obd.py` over USB instead of an adapter |

**Bench test without a car:** `%USERPROFILE%\.platformio\penv\Scripts\python.exe toolsake_obd.py COM4`
plays a ~95 s scripted drive (cold start, revs, a launch past the shift light, overheat, two stored
codes, engine off). Close the serial monitor first; the script needs the port.

The parts only the real adapter can confirm: which readings the car supports, how fast it answers,
and the adapter's own Bluetooth quirks (see TODO).

## 9. What's different from Rotor

- Board: 1.46B (412×412, SPD2010 display + touch on the shared I2C bus) instead of the 1.85"
  (360×360, ST77916 + CST816 on its own bus). Same pins otherwise.
- No radio: no navigation screens, track info, radio night mode, maintenance reminders or
  "standalone" dot. Night = 7 PM–7 AM by Otto's own clock. Music bob uses Otto's mic.
- The launch timer has no GPS speed, so it gives reaction time + peak g unless the OBD adapter
  supplies speed (section 8).
- German greetings and clips; piston character, silver + ice-blue palette.
- Own NVS namespace (`otto`) and Bluetooth UUIDs.
