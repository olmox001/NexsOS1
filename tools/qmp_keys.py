#!/usr/bin/env python3
"""Send raw qcodes into NEXS via QMP — arrows, function keys, modifiers.

qmp_type.py types TEXT; this sends KEYS by qcode name, which is the only way
to reach anything without a character (arrows, Home/End, F-keys, Esc).  Split
from qmp_type.py rather than folded into it because the two have different
inputs: one takes a string to transcribe, the other a key sequence to press.

  tools/qmp_keys.py <qmp.sock> up,up,ret [delay_first]
  tools/qmp_keys.py <qmp.sock> ctrl-c
  tools/qmp_keys.py <qmp.sock> shift-a,shift-b

A comma separates presses; a dash inside one press means "hold the modifiers
while tapping the last key", so `ctrl-c` is press ctrl, tap c, release ctrl.
Common qcodes: up down left right home end pgup pgdn ret esc tab spc backspace
delete f1..f12 ctrl shift alt.
"""
import json, socket, sys, time

if len(sys.argv) < 3:
    sys.exit(__doc__)

sock_path = sys.argv[1]
presses = [p for p in sys.argv[2].split(",") if p]
delay_first = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock_path)
f = s.makefile("rw")


def cmd(obj):
    f.write(json.dumps(obj) + "\n")
    f.flush()
    while True:
        line = f.readline()
        if not line:
            raise RuntimeError("qmp closed")
        msg = json.loads(line)
        if "return" in msg or "error" in msg:
            if "error" in msg:
                raise RuntimeError(msg["error"].get("desc", str(msg)))
            return msg


def key(q, down):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "key", "data": {"down": down,
                                 "key": {"type": "qcode", "data": q}}}]}})
    time.sleep(0.05)


f.readline()  # greeting
cmd({"execute": "qmp_capabilities"})
time.sleep(delay_first)

for press in presses:
    parts = press.split("-")
    mods, main = parts[:-1], parts[-1]
    for m in mods:
        key(m, True)
    key(main, True)
    key(main, False)
    for m in reversed(mods):
        key(m, False)
    time.sleep(0.2)

print("sent:", ",".join(presses))
time.sleep(1)
