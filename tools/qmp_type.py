#!/usr/bin/env python3
"""Type a string into NEXS via QMP input-send-event (virtio-keyboard)."""
import json, socket, sys, time

sock_path = sys.argv[1]
text = sys.argv[2]
delay_first = float(sys.argv[3]) if len(sys.argv) > 3 else 12.0

QCODE = {ch: ch for ch in "abcdefghijklmnopqrstuvwxyz0123456789"}
QCODE[" "] = "spc"
QCODE["\n"] = "ret"
QCODE["-"] = "minus"
QCODE["/"] = "slash"
QCODE["."] = "dot"

# Characters that need shift held (guest keymap is US-style: '_' = Shift+'-')
SHIFTED = {"_": "minus", ":": "semicolon", "!": "1", "?": "slash",
           '"': "apostrophe", "'": "apostrophe", "(": "9", ")": "0",
           "#": "3", "$": "4", "%": "5", "&": "7", "*": "8", "+": "equal",
           "<": "comma", ">": "dot", "|": "backslash", "~": "grave_accent",
           "^": "6", "@": "2", "{": "bracket_left", "}": "bracket_right"}
# Uppercase is Shift + the lowercase key.  Without this a path like
# /home/LUA/... died on a KeyError halfway through and typed a truncated
# command into the guest, which looks exactly like a guest-side hang.
for _c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
    SHIFTED[_c] = _c.lower()
QCODE[","] = "comma"
QCODE[";"] = "semicolon"
QCODE["="] = "equal"
QCODE["["] = "bracket_left"
QCODE["]"] = "bracket_right"
QCODE["\\"] = "backslash"
QCODE["`"] = "grave_accent"
QCODE["\t"] = "tab"

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
            return msg

f.readline()  # greeting
cmd({"execute": "qmp_capabilities"})
time.sleep(delay_first)  # wait for the shell window to be up and focused

def send_key(q, down):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "key", "data": {"down": down,
                                 "key": {"type": "qcode", "data": q}}}]}})
    time.sleep(0.05)

for ch in text:
    if ch == '\x04':
        send_key("ctrl", True)
        send_key("d", True)
        send_key("d", False)
        send_key("ctrl", False)
        time.sleep(0.15)
        continue
    if ch == '\x03':
        send_key("ctrl", True)
        send_key("c", True)
        send_key("c", False)
        send_key("ctrl", False)
        time.sleep(0.15)
        continue
    shifted = ch in SHIFTED
    q = SHIFTED[ch] if shifted else QCODE[ch]
    if shifted:
        send_key("shift", True)
    for down in (True, False):
        send_key(q, down)
    if shifted:
        send_key("shift", False)
    time.sleep(0.15)

print("typed:", repr(text))
time.sleep(1)
