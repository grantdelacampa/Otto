# Fake speed input for a bench 0-60 (Otto has no speed source of its own) (needs pyserial: use the PlatformIO penv python):
#   python tools/fake_gps.py COM3 arm|auto <0-60 secs> [logfile]
# Afterwards run `launch resetbest` so the bench time is not your record.
#  arm : sends `launch arm`, speed 0 at 10 Hz; when the board logs "launch: running" (you tilted
#        it on green), ramps the speed so 60 mph comes T seconds later, runs on to 70, then slows to 0.
#  auto: no arm; speed 0 for 6 s (Otto stages itself), then the same ramp (automatic run).
import serial, sys, time
port, mode, T = sys.argv[1], sys.argv[2], float(sys.argv[3])
logf = open(sys.argv[4] if len(sys.argv) > 4 else "fake_gps.log", "w", encoding="utf-8")
p = serial.Serial(); p.port = port; p.baudrate = 115200; p.timeout = 0
p.dtr = False; p.rts = False
p.open()
buf = ""
def pump():
    global buf
    d = p.read(1024)
    if d:
        s = d.decode("utf-8", "replace"); buf += s; logf.write(s); logf.flush()
        sys.stdout.write(s); sys.stdout.flush()
def send(c): p.write((c + "\n").encode())
def speed_at(t):            # 0 -> 60 in T s, easing off like a real car; on to 70
    if t <= 0: return 0.0
    v = 60.0 * (t / T) ** 0.8
    return min(v, 70.0)
start = time.time()
if mode == "arm": send("launch arm")
t_go = None if mode == "arm" else start + 6.0
last = 0
while time.time() - start < 120:
    pump()
    now = time.time()
    if t_go is None and "launch: running" in buf: t_go = now; print("\n>>> ramp started")
    if now - last >= 0.1:
        last = now
        if t_go is None: v = 0.0
        else:
            t = now - t_go
            tEnd = T * (70 / 60.0) ** 1.25 + 2.0        # reach 70, hold 2 s
            v = speed_at(t) if t < tEnd else max(0.0, 70 - (t - tEnd) * 14)
            if t > tEnd + 6: print("\n>>> done"); break
        send("speed %.1f" % v)
for _ in range(30): pump(); time.sleep(0.1)
