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
  - Battery: **500 mAh** LiPo (owner). `batlog` drain figures default to it; percent is voltage-only.
    Bench run-down (Nov 19 2026): ~2 h awake (~400 mV/h, ~220 mA), sleep drain too small to see.
  - No charge-status pin. On the charger the ADC reads ~4.13 V at once whatever the cell holds
    (a 4 % cell jumped 3.49 -> 4.13 V) and never passes 4.15, so `power.cpp` detects plug / unplug by
    that step (also across deep sleep, RTC_DATA_ATTR) and holds the pre-plug percent while charging.
    `Charged` ("FULL!", needs > 4.17 V) can't fire on this board. A plug-in at boot with a near-full
    cell shows no step, so a USB host (`Serial.isPlugged()`, SOF frames) also counts as charging.
  - **Power budget** (`pwrtest`, Nov 19 2026; ADC sag vs interleaved baselines, the cell's resistance
    is low so everything removable is only ~11 mV): backlight ~half of the savable draw (90 % -> 10 %
    is the biggest win; 75 % is barely cheaper than 90), frame rate ~1/4, voice ~1/6, speaker I2S
    clock a little; **CPU 240 -> 160/80 MHz and BLE advertising: too small to see** (CPU left at 240).
    Changes made: calm faces (Idle / Bored / Sleep, slow spin, no touch / menu) at 12 fps
    (`IDLE_FRAME_MS`), speaker I2S channel disabled between sounds (PCM5101 standby; no pops heard),
    BLE advertises fast 30 s after init / disconnect / menu open and slowly otherwise (owner kept it on:
    too small to measure), `wirelessOn` (menu WIRELESS) = no
    BLE stack (unless obdOn) and no Wi-Fi tuning. The drive that prompted it (Nov 19): ~1 h 53 min
    awake + 4 h 44 min asleep on one charge, ~270 mA awake; sleep drain negligible.
  - **One brightness** (owner, Nov 22 2026): NIGHT BRIGHT removed (menu, voice, tuning page); the RTC
    has no backup cell, so every power-off (incl. the 1 h auto-off) loses the clock and a night level
    rarely applied. `nightBri` stays in Settings (blob layout); `ctx.night` still drives yawns.
  - Glow (perf, Nov 19 2026): the strength costs nothing (only colour); glow 0 saves ~2.4 of ~35 ms
    draw. Owner's brightness: 10 % day + night (also the defaults now).
  - **Driving fixes** (owner's drive, Nov 19 2026), all IMU-only (no speed): `stoppedSecs` counts up
    while cruising straight too, so (1) road shake made Rev faces, (2) a smooth idle at a light read as
    engine off -> drive summary at every stop, (3) the music bob dropped out (false revs + road rumble
    in the beat's bass band). Now: `quietSecs` (gyro ~1 s LP < 1 dps and vertical 0.5-3 Hz road RMS
    < 12 mg) = really stopped; revs need 3 s quiet first; engine off needs vib < 0.3 x engineVib with a
    quiet body for 8 s; the summary waits 20 s and needs him still parked; the music bob has
    hysteresis (looser limits once bobbing, 8 s to stop). Thresholds are guesses: **`drivelog`**
    (/drive.csv, a line every 2 s while not still: vib, road, gyro, stopped, quiet, engine, face,
    music level / background / beat / bob, + event markers) is there to tune them from a real drive.
    Rotor has the same stoppedSecs logic and probably the same bugs.
    `gyroLP` is rotation *wobble* (|gyro - its 5 s average|): the raw gyro carried a 7.6 dps offset on
    the bench (the offset only re-learns while |lat| < 0.05 g, i.e. mounted level), which made
    "quiet" impossible. Desk: wobble 0.27 dps, vib 0.0001-0.0002 g.
  - **ENGINE LEARN** (menu / `enginelearn`, motion.cpp state machine): idle in P 20 s (vib, road,
    wobble), wait for the engine to stop (vib < half idle, 1.5 s; tap = skip -> floor 0.0006 g), off
    10 s. Saves idleVib, engineVib = sqrt(off x idle), engOffVib = sqrt(off x on), quietGyro / Road =
    2.5 x idle (clamped). Refuses (saves nothing, "?" card) if idle < 2.5 x off. The sleep peek's
    vibration wake uses max(0.7 x engineVib, 2.5 x engOffVib, 0.003 g) so a low learned level can't
    wake him on nothing. Card = Expr::EngineLearn (OV_ANSWER + OV_RING), taps go to the routine.
  - **Learn the road** (voice "learn the road" / "learn driving", hand-encoded phonemes `LkN jc RbD`,
    `LkN DRiVgl`; also "learn the engine"; serial `enginelearn road`): 20 s at ~10 Hz while cruising,
    the 20th percentile of wobble / road -> cruiseGyro / cruiseRoad. `recomputeQuiet()`: quiet =
    sqrt(idle x cruise) when cruise > 1.5 x idle, else 2.5 x idle; does nothing until ENGINE LEARN
    has measured idleGyro / idleRoad (a road learn against a default idle saved junk on the bench).
    Refuses when no shakier than idle. Held in the hand: wobble 1.1 dps; still bench 0.06.
    The wobble's 5 s average is seeded from the first sample (from zero it read ~1 dps for ~15 s).
  - **Status icons** (`faceSetStatus`, FACE_ST_* bits; lower left 2x2 at px (62|90, 268|298)): mic =
    voiceRunning, BT = bleAnyConnected (not advertising: that's nearly always), Wi-Fi = tuning on,
    OBD (yellow plug) = obdRpmFresh. Drawn on OV_HOUSING faces; a change forces one full frame.
  - **Schematic** (docs/ESP32-S3-Touch-LCD-1.46.pdf, owner, Nov 20 2026): TCA9554 INT (pin 13) goes to
    R42, a 3-pad either-or footprint: pad 2 = 3V3 (pull-up), pad 3 = GPIO0 (BOOT, a deep-sleep wake
    pin). Fitted as the 10K pull-up: `womint` (bench) saw the IMU's WoM toggle INT1 on the expander
    (EXIO5) with every knock but GPIO0 never went low. Moving / bridging R42 to the GPIO0 side would
    let a knock wake him directly (no 4/15 s peeks). Caveats for that mod: BOOT and the alarm then
    share GPIO0 (read the expander to tell them apart; the INT stays low until the expander's input
    register is read, so clear it before sleeping); BOOT-held wakes already boot normally, so a low
    GPIO0 at a deep-sleep wake is fine.
    The **NS8002 speaker amp** runs from VCC (battery) with its SD pin tied to ground: always on, even
    in deep sleep (a few mA, part of the sleep floor; no firmware control). Mic EN tied to 3V3 too.
  - **Sleep result** (overnight bench, Nov 21 2026, after 20 min awake to settle): 3.84 -> 3.63 V in
    18.1 h (~11 mV/h, roughly ~10 mA), about half the first night's rate; 4457 peeks (as expected).
    The rest is hardware (always-on NS8002 amp, display panel, ...). So: **auto power-off after 1 h
    asleep** (`AUTO_OFF_SECS`, `autoPowerOff()` in quickWakeCheck: logs `wake-off` with the cell
    voltage, then boardPowerOff(); PWR turns him back on; on USB he just keeps sleeping). Bench:
    `autooff <secs>` sets a shorter limit until the next power-off (RTC memory; a port-open reset
    clears it, so keep the port open until the unplug). Bench Nov 21: off 124 s into a 120 s test
    (`wake-off` logged), stayed off until PWR, then a cold boot.
  - **Music false starts** (owner, Nov 21 2026): room noise reads steady 1.00 and a faint ~174 BPM tick
    sat at beat ~0.16 (limit 0.20), so any +6 dB (TV, talking) could start the bob. Starting now
    also needs a steady tempo: 4 checks in a row within 8 %.
  - **Display chip asleep in deep sleep** (Nov 20 2026): displayOff() now sends SLPIN (0x10, QSPI
    write opcode 0x02 like the driver's tx_param; the driver has no disp_sleep) after DISPOFF, and
    enterDeepSleep() holds TP_RST + LCD_RST (EXIO1/2) low. boardEarlyInit() keeps them low after a
    deep-sleep wake (`esp_reset_reason() == ESP_RST_DEEPSLEEP`), so peeks don't start the SPD2010;
    displayInit() / touchInit() pulse their resets on a full boot. Bench: BOOT wake -> display +
    touch fine. Saving not measured yet (overnight bench run).
  - **Sleep peeks** (Nov 20 2026): overnight on the bench 3.99 -> 3.78 V in 9.4 h (~15 mA asleep).
    Each 4 s peek is a full chip start-up incl. the PSRAM self-test. Now 4 s for the first 15 min
    parked (`s_peeks < 225`), then 15 s (`peekSecs()`). Verified: 1343 peeks in 4.98 h (expected ~1359).
    batlog on wake: `wake-pk` = the cell voltage from the last full peek (RTC `s_peekMv`, a reading
    under 4.11 V wins), logged before the wake line so the sleep drain uses it even when the charger
    went in before the wake (that ruined a 5 h test); then `peeks<N>`.
  - Settings v5 appended idleGyro / idleRoad / cruiseGyro / cruiseRoad (v4 ended on a float).
  - Settings v4 appended driveLogOn + the learned values. v3 ended on a bool, so its blob has tail
    padding: settingsLoad copies only `V3_USED` bytes (offsetof(wirelessOn) + 1).
  - **Voice naps** (owner, Nov 19 2026): "Hi ESP" listens 5 min (`VOICE_WINDOW_MS`) after a wake,
    engine start, touch, menu or any use, then ESP-SR is stopped until the next one. The music bob
    was fed only by ESP-SR's mic reads, so a `micbob` task (voice.cpp, core 1 prio 1) reads the mic
    while ESP-SR is off (also with VOICE off: the bob works without voice now). micRead isn't
    reentrant: start() raises `s_srStarting` and waits for the task's block. A small ice-blue mic
    on the lower left shows while it listens (see Status icons).
  - Settings blob: SETTINGS_VERSION 3 only **appended** `wirelessOn`; settingsLoad carries a v2 blob
    over (`V2_SIZE = offsetof(Settings, wirelessOn)`) so the mount calibration survives. Append new
    fields at the end the same way instead of resetting everyone's settings.
  - SPD2010 wants window edges on multiples of 4: `display.cpp` pushes full-width 32-row stripes.
  - **Touch runs in its own task** (`readerTask` in touch.cpp, core 1, prio 2, every 10 ms) feeding
    time-stamped samples to the gesture code. The SPD2010 shows a lift only as an idle status read
    (`00 08`, no INT pulse), and the main loop is ~60-100 ms per frame, so fast taps merged into one
    long touch (petting) when it was read from the loop. `touch raw [secs]` prints every status read.
  - **Frame rate** (Nov 15 2026; idle with voice on 12 -> ~26 fps, cap is 30): the PSRAM canvas is
    bandwidth-bound (~23 MB/s with voice; a faster fill didn't help, restoring from a saved background
    was slower). So: (1) piston body cached in a sprite, redrawn only when lean / size / hue changes,
    brightness applied on copy via a 64-entry table (idle breathes); (2) "plain" faces (no overlays
    but OV_HOUSING) clear, draw and send only the piston + rod + crank box (`partBox()`, canvas clip,
    `displayPushRect`, edges on multiples of 4); anything else is a full frame; (3) cylinder walls =
    rects, crank dashes = triangles. `perf` shows draw parts + cache redraws. Anything drawing on the
    canvas outside faceRender must call `faceForceFull()` (the menu does).
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
- Tuning page style (Nov 15 2026): Otto's palette, system fonts (no Google Fonts: Otto's Wi-Fi has no
  internet), thin rules, 8 px corners - a calm 90s-Mercedes look instead of Rotor's 80s pixel font +
  hazard stripe. No cabin-faces toggle (no SHTC3).
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
- **Low battery** (owner, Nov 22 2026): the "PLUG ME IN" card once each at 15 / 8 / 4 % (was every 90 s;
  a wake re-warns once at the current level, charging re-arms). Idle / Bored look tired below 15 %
  (heavy lids), 8 % (sad brows, dimmer), 4 % (gray); not while charging (on a USB host = charging).
- **Music in the car** (drive log, Nov 22 2026): the cabin reads -8..-25 dB and the V8 idling gave a
  steady -22 dB with beat 0.3-0.45, while the background sat at ~-58 from a quiet moment and crept up
  only 2.4 dB/min, so idle rumble bobbed him. The background now rises 1 dB/s while it's loud with no
  music, and no bob starts in the first 30 s after a wake. Staying on needs 5 of the last 16 checks
  on tempo (an office's tempo hopped 81-157 BPM). Absolute floor -52 dBFS to start, -55 to stay.
- **Car noise learn** (Nov 22 2026, settings v6 appended idleNoiseDb / idleBeat / idleBpm / cruiseNoiseDb;
  not yet run in the car, v5 -> v6 migration not yet seen on the board): ENGINE LEARN's idle phase and
  learn the road also sample the music analysis every 500 ms (needs MUSIC BOB on: that's what runs it).
  Idle: median level + beat, and the tempo a third of the estimates agree on (6 %) = the V8's rhythm.
  Road: 30th percentile level. music.cpp: with the engine on, the background never sits below the
  learned noise (idle when quietSecs > 1, else max(idle, cruise)); at idle a tempo near idleBpm (x1,
  x2, x0.5) isn't music; a learned value skips the 30 s wake wait. No "music heard" refusal during a
  learn on purpose (the idle rumble is what fooled the detector); the cards say MUSIC OFF.
  Owner's goal: Otto tunes himself; no one should have to read logs.
- **Phone clock** (ble.cpp, branch `phone-clock`, Nov 22 2026; **works on the owner's iPhone**: keyboard unaffected, iOS reconnects him on wake; paired
  from Settings > Bluetooth, read 14:45 tz -480): the RTC has no backup cell. iOS Settings lists only
  devices with a service it uses itself (an ANCS solicitation did NOT get him listed), so Otto is also
  a **HID consumer-control remote** (volume up / down bits, never sends a report; appearance 0x0180,
  battery service = his percent). Advertising = flags + appearance + HID 0x1812 + CTS solicitation;
  name + Otto's UUID in the scan response. Advertising data set once in bleInit never went out: it's
  set before every start now (`setAdvData`). On each connect Otto acts as GATT client on the peer's own link (raw
  `ble_gattc_*`; NimBLEClient can't adopt a peer-opened link): find CTS, read Local Time Info (0x2A0F,
  tz + DST in 15 min) and Current Time (0x2A2B, local), apply in the main loop (RTC shares I2C).
  Insufficient auth -> `startSecurity` once (Just Works bonding, NVS-persisted); a private-address peer
  with no visible CTS also gets one pairing try (PCs use fixed addresses: no prompt). Phone links
  slow to 0.5-1 s intervals unless telemetry is subscribed. `bleTunerConnected()` (telemetry
  subscribed) now gates sleep and the menu's TUNER row; `bleAnyConnected()` still drives the BT icon.
  Serial `phone` / `phone forget`.
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
  `-Lang de-DE`). Generated Nov 15 2026 with **Microsoft Stefan** (owner's pick; Katja / Hedda also
  installed), 25 clips, ~1.4 MB, on the board via `pio run -t uploadfs` (wipes the battery + drive
  logs; settings live in NVS, speech models in their own partition). Heard on the speaker: good.
