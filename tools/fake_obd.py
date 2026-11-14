# Fake ELM327 + car for testing Otto's OBD-II code on the bench (no adapter, no car).
# Needs pyserial: use the PlatformIO penv python:
#   %USERPROFILE%\.platformio\penv\Scripts\python.exe tools\fake_obd.py COM4 [speedup]
#
# Sends `obd sim on`; Otto then writes each adapter request as "obd> 010C" and this answers
# "obd< 41 0C 0A F0" like a real ELM327 on an ISO 9141 car would (with a small delay).
# A scripted drive (seconds, at speedup 1):
#    0  ignition off: the car doesn't answer ("NO DATA")
#    5  engine start, cold (coolant 22 C and rising fast) -> ENGINE ON, then the warm-up face
#   14  two revs while parked                               -> rev faces, piston pumps with rpm
#   24  launch 0-60 in ~7 s, through the gears              -> shift light at 5800 rpm, speed feed
#   32  cruise at 60 mph, then slow to a stop by 52         -> coolant passes 75 C -> "warm"
#   55  coolant spikes to 118 C for 10 s                    -> engine hot face
#   80  engine off                                          -> engine off, car silent again
# The car also has its check-engine lamp on with two stored codes (P0171, P0300), shown ~11 s
# after the start. Afterwards: `obd sim off`, and `launch resetbest` if a bench run set a "best".
import serial, sys, time

port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
speedup = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
END = 95

# supported PIDs 01-20: 01 03 04 05 06 07 0C 0D 0E 0F 10 11 13 15 1C (no 20 -> nothing past 0x20)
SUPPORTED = [0x01, 0x03, 0x04, 0x05, 0x06, 0x07, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x13, 0x15, 0x1C]
bits = 0
for pid in SUPPORTED:
    bits |= 1 << (32 - pid)


def lerp(a, b, k):
    return a + (b - a) * max(0.0, min(1.0, k))


def car(t):
    """(running, rpm, kmh, coolant C, throttle %) at scenario time t."""
    if t < 5 or t >= 80:
        return False, 0, 0, 22 if t < 5 else 92, 0
    cool = min(90, 22 + (t - 5) * 3.0)              # warms up absurdly fast, for the demo
    if 55 <= t < 65:
        cool = 118
    rpm, kmh, thr = 700, 0, 12
    if t < 6:
        rpm = lerp(250, 1200, t - 5)                # cranking, flare
    elif t < 9:
        rpm = lerp(1200, 700, (t - 6) / 3)
    elif 14 <= t < 15.5:
        rpm, thr = lerp(800, 3600, (t - 14) / 0.6), 60
    elif 17 <= t < 18.5:
        rpm, thr = lerp(800, 4200, (t - 17) / 0.6), 70
    elif 24 <= t < 31:                              # launch: 0-97 km/h in 7 s, 3 gears
        k = (t - 24) / 7
        kmh = 97 * k ** 0.85
        gear_k = (k * 3) % 1
        rpm, thr = lerp(2200, 6000, gear_k), 95
    elif 31 <= t < 45:
        kmh, rpm, thr = 97, 2300, 25
    elif 45 <= t < 52:
        kmh = lerp(97, 0, (t - 45) / 7)
        rpm, thr = lerp(2300, 750, (t - 45) / 7), 5
    return True, rpm, kmh, cool, thr


def answer(cmd, t):
    c = cmd.strip().upper().replace(" ", "")
    if c == "ATZ":
        return "ELM327 v1.5"
    if c == "ATDP":
        return "ISO 9141-2"
    if c.startswith("AT"):
        return "OK"
    running, rpm, kmh, cool, thr = car(t)
    ign = 5 <= t < 80
    if not ign:
        return "SEARCHING...|UNABLE TO CONNECT" if c == "0100" else "NO DATA"
    if c == "0100":
        return "SEARCHING...|41 00 %02X %02X %02X %02X" % (bits >> 24 & 255, bits >> 16 & 255, bits >> 8 & 255, bits & 255)
    if c == "03":
        return "43 01 71 03 00 00 00"                  # P0171 (lean), P0300 (misfire)
    if c.startswith("01") and len(c) == 4:
        pid = int(c[2:], 16)
        if pid not in SUPPORTED:
            return "NO DATA"
        if pid == 0x01:
            return "41 01 82 07 65 04"                 # lamp on, 2 codes
        if pid == 0x0C:
            v = int(rpm * 4)
            return "41 0C %02X %02X" % (v >> 8, v & 255)
        if pid == 0x0D:
            return "41 0D %02X" % int(kmh)
        if pid == 0x05:
            return "41 05 %02X" % int(cool + 40)
        if pid == 0x11:
            return "41 11 %02X" % int(thr * 255 / 100)
        if pid == 0x10:
            maf = int((2.5 + rpm / 1000 * thr / 10) * 100)   # rough g/s * 100
            return "41 10 %02X %02X" % (maf >> 8, maf & 255)
        return "NO DATA"
    return "?"


p = serial.Serial()
p.port, p.baudrate, p.timeout = port, 115200, 0.02
p.dtr = False
p.rts = False                                         # don't reset the board (native USB)
p.open()
p.write(b"obd sim on\n")
t0 = time.time()
buf = b""
asked = 0
print("fake_obd: running the scripted drive (%.0f s)" % (END / speedup))
while (time.time() - t0) * speedup < END:
    buf += p.read(512)
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        s = line.decode("utf-8", "replace").strip()
        if s.startswith("obd> "):
            t = (time.time() - t0) * speedup
            time.sleep(0.06)                          # ISO 9141 is slow
            rep = answer(s[5:], t)
            p.write(("obd< " + rep + "\n").encode())
            asked += 1
        elif s.startswith(("obd", "motion", "launch")):
            print("%6.1f  %s" % ((time.time() - t0) * speedup, s))
p.write(b"obd\n")
time.sleep(0.4)
print(p.read(2000).decode("utf-8", "replace"))
p.write(b"obd sim off\n")
print("fake_obd: done, %d requests answered" % asked)
