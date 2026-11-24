"""Otto drive report: the same numbers after every drive, so tuning changes can be compared.

  Pull from Otto and save:   python tools/drivereport.py COM4
  Re-read a saved pull:      python tools/drivereport.py docs/tuning/drives/2026-11-23.txt
  Options: --day YYYY-MM-DD  report that day's sessions (default: the newest day in the log)
           --all             every session in the log
           --nosave          don't write the pull to docs/tuning/drives/

A pull saves the raw `drivelog` + `get` output as docs/tuning/drives/<date>.txt (an existing file
for the day gets -2, -3, ...). Run it with PlatformIO's Python (it has pyserial):
  %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe tools/drivereport.py COM4

Log columns (drivelog.cpp): t, vib (0.1 mg), road (0.1 mg), gyro wobble (0.1 dps), stopped s,
quiet s, engine, face, music level dB, background dB, beat x100, bobbing; tune 2+: tempo estimate
BPM, steady x100.
"""
import datetime as dt
import json
import os
import re
import statistics as st
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAVE_DIR = os.path.join(ROOT, "docs", "tuning", "drives")
TZ_OFFSET_H = -8          # local time for the report (Otto logs UTC from his RTC)
RESTART_S = 120           # an engine "off" followed by "on" within this = false off
DRIVE_MIN_ROWS = 60       # a session with fewer rows (2 s each) is a hop / bench, not a drive


# ------------------------------------------------------------------ pull --
def pull(port):
    import serial
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = False         # native USB: DTR / RTS double as reset
    s.rts = False
    s.open()
    time.sleep(0.5)
    s.reset_input_buffer()
    out = []
    for cmd in ("drivelog", "get"):
        s.write((cmd + "\n").encode())
        buf, quiet = b"", 0
        while quiet < 15:
            d = s.read(65536)
            if d:
                buf += d
                quiet = 0
            else:
                quiet += 1
        out.append(buf.decode("utf-8", "replace"))
    s.close()
    return "\n".join(out)


def save(text):
    os.makedirs(SAVE_DIR, exist_ok=True)
    day = dt.datetime.now().strftime("%Y-%m-%d")
    path = os.path.join(SAVE_DIR, day + ".txt")
    n = 2
    while os.path.exists(path):
        path = os.path.join(SAVE_DIR, f"{day}-{n}.txt")
        n += 1
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text.replace("\r\n", "\n"))
    return path


# ----------------------------------------------------------------- parse --
def parse(text):
    sessions, cur, settings = [], None, None
    for line in text.replace("\r", "").split("\n"):
        line = line.strip()
        if line.startswith("{") and '"brake"' in line:
            try:
                settings = json.loads(line)
            except ValueError:
                pass
            continue
        m = re.match(r"# wake (\d+) \(rtc\), (\d+) mV(?:, tune (\S+))?", line)
        if m:
            cur = {"utc": int(m.group(1)), "mv": int(m.group(2)), "tune": m.group(3) or "-",
                   "rows": [], "ev": []}
            sessions.append(cur)
            continue
        if cur is None:
            continue
        if line.startswith("# "):
            p = line.split(None, 2)
            if len(p) == 3 and p[1].isdigit():
                cur["ev"].append((int(p[1]), p[2]))
        elif line[:1].isdigit():
            f = line.split(",")
            if len(f) in (12, 14):          # tune 2+ adds tempo estimate BPM, steady x100
                try:
                    cur["rows"].append([f[7] if i == 7 else int(x) for i, x in enumerate(f)])
                except ValueError:
                    pass
    for s in sessions:
        s["local"] = dt.datetime.fromtimestamp(s["utc"], dt.timezone.utc) + dt.timedelta(hours=TZ_OFFSET_H)
        # A clock that wasn't set (no phone yet) reads early 2026 or 1970: flag it
        s["clock_ok"] = s["local"].year >= 2026 and not (s["local"].month == 1 and s["local"].day <= 3)
    return sessions, settings


# ---------------------------------------------------------------- report --
def med(xs):
    return st.median(xs) if xs else float("nan")


def pct(xs, q):
    if not xs:
        return float("nan")
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))]


def engine_events(s):
    """Engine offs with what followed: restart time and whether a summary showed in between."""
    ev = s["ev"]
    learn_t = [t for t, e in ev if e.startswith("learn idle")]
    out = []
    for i, (t, e) in enumerate(ev):
        if e != "engine off":
            continue
        nxt_on = next((t2 for t2, e2 in ev[i + 1:] if e2 == "engine on"), None)
        summary = any(e2 == "summary" and (nxt_on is None or t2 < nxt_on) for t2, e2 in ev[i + 1:])
        in_learn = any(0 <= lt - t <= 90 for lt in learn_t)   # the learn asks for the engine off
        out.append({"t": t, "back_on": None if nxt_on is None else nxt_on - t,
                    "summary": summary, "learn": in_learn})
    return out


def report(sessions, settings, day=None, show_all=False):
    good = [s for s in sessions if s["clock_ok"]]
    if not show_all:
        if day is None and good:
            day = max(s["local"].date() for s in good).isoformat()
        sessions = [s for s in good if s["local"].date().isoformat() == day]
    print(f"Otto drive report: {'all sessions' if show_all else day}\n")

    drives = [s for s in sessions if len(s["rows"]) >= DRIVE_MIN_ROWS]
    tot = {"false_off": 0, "false_stop": 0, "real_off": 0, "drive_min": 0.0}
    stop_vib_on, vib_off, bob_rows, rows_all, music_gap, beats = [], [], 0, 0, [], []
    revs = 0
    why = {}
    tempo_jumps, tempo_n, steady = 0, 0, []

    print("Sessions (drives = at least 2 min of log):")
    for s in sessions:
        r = s["rows"]
        if not r:
            continue
        t0 = r[0][0]
        mins = (r[-1][0] - t0) / 60
        kind = "drive" if s in drives else "short"
        print(f"  {s['local']:%H:%M}  {mins:5.1f} min  {kind:5}  tune {s['tune']:>3}  {s['mv']} mV")
        if s not in drives:
            continue
        tot["drive_min"] += mins
        for e in engine_events(s):
            rel = e["t"] - t0
            if e["learn"]:
                continue
            if e["back_on"] is not None and e["back_on"] <= RESTART_S:
                tot["false_off"] += 1
                if e["summary"]:
                    tot["false_stop"] += 1
                print(f"      false engine off at {rel // 60}:{rel % 60:02d}, back on {e['back_on']} s later"
                      f"{'  << SUMMARY SHOWN' if e['summary'] else ''}")
            else:
                tot["real_off"] += 1
        revs += sum(1 for _, e in s["ev"] if e == "rev")
        for _, e in s["ev"]:
            if e.startswith("off: "):
                why[e[5:]] = why.get(e[5:], 0) + 1
        prev = None
        for x in r:
            if len(x) == 14 and x[5] == 0:   # moving: does the tempo estimate hold still?
                steady.append(x[13])
                if prev:
                    tempo_n += 1
                    tempo_jumps += abs(x[12] - prev) > prev * 0.08
                prev = x[12] or None
        for t, e in s["ev"]:
            if e.startswith(("learn", "road learn")):
                print(f"      {e} ({(t - t0) // 60}:{(t - t0) % 60:02d})")
        # Stops with the engine running vs. the engine off at the end
        last_off = max((t for t, e in s["ev"] if e == "engine off"), default=None)
        last_on = max((t for t, e in s["ev"] if e == "engine on"), default=None)
        for x in r:
            rows_all += 1
            bob_rows += x[11]
            if x[11] == 0:
                music_gap.append(x[8] - x[9])
            beats.append(x[10])
            if x[5] >= 3 and x[6] == 1:
                stop_vib_on.append(x[1])
            if last_off and (last_on is None or last_off > last_on) and x[0] > last_off + 3:
                vib_off.append(x[1])

    print()
    print(f"Driving logged: {tot['drive_min']:.0f} min in {len(drives)} drives")
    print(f"Engine off at a stop (false):  {tot['false_off']}   of which a drive summary showed: {tot['false_stop']}")
    print(f"Engine off for real:           {tot['real_off']}")
    if why:
        print("Why engine off (engine not felt): " + ", ".join(f"{k} {v}" for k, v in why.items()))
    print(f"Revs: {revs}")
    print(f"Stopped, engine counted on:    vib median {med(stop_vib_on) / 10:.2f} mg  (n={len(stop_vib_on)})")
    print(f"Parked, engine off at the end: vib median {med(vib_off) / 10:.2f} mg  (n={len(vib_off)})")
    print(f"Music bob: {bob_rows} of {rows_all} lines ({100 * bob_rows / max(1, rows_all):.1f} %)")
    print(f"Music level over background (not bobbing): median {med(music_gap):+.0f} dB, "
          f"90th pct {pct(music_gap, 0.9):+.0f} dB  (a start needs about +6)")
    print(f"Beat strength: median {med(beats) / 100:.2f}, 90th pct {pct(beats, 0.9) / 100:.2f}")
    if tempo_n:
        print(f"Tempo estimate (moving): moved > 8 % in {100 * tempo_jumps / tempo_n:.0f} % of 2 s steps "
              f"(music should hold it); steady median {med(steady) / 100:.2f}")
    if settings:
        keys = ["engineVib", "engOffVib", "idleVib", "quietGyro", "quietRoad", "idleGyro", "idleRoad",
                "cruiseGyro", "cruiseRoad", "idleNoiseDb", "idleBeat", "idleBpm", "cruiseNoiseDb", "engineFelt"]
        print("\nLearned / tuned values now on Otto:")
        print("  " + ", ".join(f"{k} {settings[k]:.4g}" for k in keys if k in settings))


def main():
    args = sys.argv[1:]
    day = None
    if "--day" in args:
        i = args.index("--day")
        day = args[i + 1]
        del args[i:i + 2]
    show_all = "--all" in args
    nosave = "--nosave" in args
    args = [a for a in args if not a.startswith("--")]
    if not args:
        print(__doc__)
        return
    src = args[0]
    if os.path.exists(src):
        text = open(src, encoding="utf-8", errors="replace").read()
    else:
        text = pull(src)
        if not nosave:
            print(f"saved {os.path.relpath(save(text), ROOT)}\n")
    sessions, settings = parse(text)
    report(sessions, settings, day, show_all)


if __name__ == "__main__":
    main()
