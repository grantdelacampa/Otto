# Otto tuning log

Each tuning change to the driving / music detection is tied to the drive that showed the problem and
the drive that checks the fix.

## After a drive

1. Plug Otto in and run the report. It saves the raw log to `drives/<date>.txt`:
   `%USERPROFILE%\.platformio\penv\Scripts\python.exe tools/drivereport.py COM4`
2. Add a row to the scoreboard and an entry below: the car, what you noticed, the report's numbers.
3. A change to the detection = bump `TUNE_REV` (src/config.h) and write what it should fix under
   "Check next drive". The log stamps every session with its tune number.
4. On the next drive, the report's numbers for that tune say whether it worked.

## Scoreboard

| Date | Car | Tune | Driving | False engine off | False summary | Music bob | Notes |
|---|---|---|---|---|---|---|---|
| 2026-11-23 | Civic | 1* | 78 min, 7 drives | 6 | 3 (owner saw 2) | 0.4 % (no reaction) | baseline; idle not felt by the IMU |
| next | Civic | 2 | | | | | re-run ENGINE LEARN first |

\* Logged before the tune stamp existed. Tune 1 is the same detection.

---

## 2026-11-23: Civic, tune 1 (baseline)

**Car:** Honda Civic (the test car, not the E420). Everything learned today is the Civic's: re-run
ENGINE LEARN and learn the road in the E420.

**Owner:** No music reaction at all. Two false stops. Otherwise good.

**Report** (`drives/2026-11-23.txt`):
- Engine off at a stop (false): 6. A drive summary showed for 3 of them (back on 66, 37 and 32 s later).
  The short ones came back on 5-8 s later.
- Vibration while stopped with the engine counted on: median **0.10 mg**. Parked with the engine off:
  **0.10 mg**. With the board in its mount, the IMU can't feel this engine idling at all.
- ENGINE LEARN saved idle 0.0036 g / off 0.0002 g. The log around it shows the idle at ~0.1-0.9 mg.
  The learn averages its idle samples, so one spike (about 9 mg at the start, probably the menu tap)
  made a silent idle look 18x the engine-off level. With a median it would have refused
  (idle < 2.5 x off).
- Music: the level stayed within +1 dB of the background (median, 90th percentile +5; a start needs
  about +6). Beat strength median 0.15. Learned cabin noise: idle -22.9 dB, cruise **-15.2 dB**.
  While driving the background never goes below the learned cruise noise, and the cabin read about
  -16 to -11 dB. Music only a little louder than -15 never stood out.
- Two road learns: wobble 1.81 / road 0.0068 (5:18), then 1.26 / 0.0197 (10:18; this one is saved).

**Why the false stops:** the engine-off rule is vibration-only. When the idle reads the same as
engine off, every stop of ~10 s turns the engine "off" and every stop of ~30 s shows a summary.
Lowering a threshold can't fix that.

**Ideas, not done yet:**
- ENGINE LEARN: median instead of mean. If idle < 2.5 x off, save "can't feel the engine" instead of
  refusing, so he switches to a stop rule that doesn't need vibration (summary only after a few
  minutes still, or after the shake of getting out).
- Learn the road: don't save a cruise noise learned while music plays (beat / tempo check), or cap it
  against the idle noise.

**Answers (owner):** music was on only for the 19:13 drive, so the road learn was clean: the
Civic really read -15 dB cruising, because the **defroster was on**. Not sure about the 17:33 drive.

With music on (19:13) the cabin was ~5 dB louder than without (median -11 vs -16 dB), but the beat
strength was the same (0.14 vs 0.15): the beat detector didn't pick the music out of road + fan noise.
The background also rose with it (-13 vs -15), eating most of the 5 dB.

Cabin level at the stops (the mic hears what the IMU can't): engine running at a light -15..-23 dB,
real engine off -45..-58 dB. Measured from 2 s into each stop, real offs fell 17+ dB and running stops
9 dB at most. Three stops in the 17:33 drive and the start of 17:43 were already quiet (~-44 dB),
so the engine may really have been off there.

**Side notes:**
- The 21:41 session (5 min) is Otto on the bench. The report counts it as a drive.
- `batlog` says "awake 200657 h": the clock jumped from Jan 2 (unset) to Nov 22, and the awake total
  counts across the jump.

---

## Tune 2 (2026-11-23): stops listen, learn uses medians

**Changes:**
- ENGINE LEARN takes the **median** of ~10 samples a second instead of the mean (a knock can't make
  a silent idle look like an engine any more).
- If the learn finds idle and off too close to tell apart *with the engine really turned off*, it
  saves **engineFelt = 0** (settings v8; card "EAR / CAN'T FEEL IT - STOPS LISTEN") instead of saving
  nothing. Then:
  - Engine off at a stop also needs the cabin to go quiet: 12 dB under its level 2 s into the stop,
    held 8 s. Or a long still (75 s at your 2 min sleep time, so the summary still comes before he sleeps), if the mic is off (MUSIC BOB off) or the car was already quiet.
    The drive log marks which one: `off: cabin went quiet` / `off: long still`.
  - Revs from vibration are off (there's no idle to rev above).
- Music: the background may sit up to 6 dB **below** the learned car noise (was: never below it), so
  learning with the fan on high doesn't leave him deaf to quieter music with the fan off.
- Drive log: two new columns, the tempo estimate (BPM) and steadiness, so the next drive shows
  whether music holds a steady tempo even when the beat strength doesn't stand out.

**Before the next drive:** run **ENGINE LEARN** in the Civic (in P, music off, then turn the engine
off when asked). It should end on "EAR / CAN'T FEEL IT". Leave MUSIC BOB on (that's what runs the mic).

**Check next drive:**
- False engine off / false summary: should drop to ~0. Look at `Why engine off`: mostly "cabin went
  quiet", and "long still" only where the car was quiet already (or a light longer than 75 s: watch for that).
- Real engine offs: still caught (the summary may come up to ~2 min late on a quiet arrival).
- Music: play some, note roughly when, and say whether the defroster was on. Compare level over
  background, beat strength and the new tempo numbers with and without music.
- Not expected to fix music by itself: the beat didn't stand out in tune 1. Tune 2 is mostly there to
  measure it.
