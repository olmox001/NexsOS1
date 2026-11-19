#!/usr/bin/env python3
"""Verify independent graphical terminals on a disposable, already booted VM."""
import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import time

import nxadb


@dataclass(frozen=True)
class Window:
    id: int
    owner: int
    x: int
    y: int
    width: int
    height: int


ROW = re.compile(r"^(\d+)\s+(\d+)\s+focus\s+pos=(-?\d+),(-?\d+)\s+"
                 r"surface=\d+x\d+\s+draw=(\d+)x(\d+)\s+", re.M)


class Guest:
    def __init__(self, qmp, uart, timeout):
        self.qmp = qmp
        self.uart = uart
        self.timeout = timeout

    def command(self, command, expected, timeout=None):
        offset = self.uart.stat().st_size
        nxadb.qmp_type(self.qmp, command + "\n", delay=0.05)
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        with self.uart.open("rb") as stream:
            while time.monotonic() < deadline:
                stream.seek(offset)
                output = stream.read().decode(errors="replace").replace("\r", "")
                match = expected.search(output)
                if match:
                    return match
                time.sleep(0.1)
        raise TimeoutError(f"{command!r}: guest did not produce {expected.pattern!r}: {output[-600:]!r}")

    def focused(self):
        return Window(*map(int, self.command("nxwins", ROW).groups()))

    def shell_pid(self):
        return int(self.command("id", re.compile(r"pid=(\d+) level=user caps="))[1])

    def echo(self, word):
        command = "echo " + word
        start = self.uart.stat().st_size
        self.command(command, re.compile(r"(?:^|> )" + command + r"\n" + word + r"\n"))
        # The next shell response is a barrier for the same TTY port.
        self.shell_pid()
        with self.uart.open("rb") as stream:
            stream.seek(start)
            lines = stream.read().decode(errors="replace").replace("\r", "").splitlines()
        if lines.count(word) != 1 or sum(line.endswith(command) for line in lines) != 1:
            raise RuntimeError(f"unexpected echo or command multiplicity: {lines}")

    def redirected_echo(self, word):
        path = "/tmp/" + word
        prompt = re.compile(r"\x1b\[32mNXShell")
        self.command("echo " + word + " > " + path, prompt)
        self.command("/bin/cat " + path, re.compile(r"^" + re.escape(word) + r"$", re.M))
        self.command("/bin/unlink " + path, prompt)

    def screenshot(self, path):
        path.parent.mkdir(parents=True, exist_ok=True)
        sock = nxadb.qmp_connect(self.qmp)
        try:
            response = nxadb.qmp_send(sock, {"execute": "screendump", "arguments": {
                "filename": str(path.resolve()), "device": nxadb.QMP_INPUT_DISPLAY, "head": 0}})
            if "error" in response:
                raise RuntimeError(response["error"])
        finally:
            nxadb.qmp_close(sock)
        with path.open("rb") as capture:
            if capture.readline().strip() != b"P6":
                raise RuntimeError(f"missing QEMU framebuffer in {path}")

    def click_at(self, x, y):
        nxadb.qmp_mouse_rel(self.qmp, -8192, -8192)
        time.sleep(0.2)
        nxadb.qmp_mouse_rel(self.qmp, x, y, "left")
        time.sleep(0.3)

    def click(self, target, front=None):
        # Hit the client area, avoiding title-bar controls and resize grips.
        for x in range(target.x + 16, target.x + target.width - 16, 16):
            for y in range(target.y + 40, target.y + target.height - 16, 16):
                if front and (front.x - 8 <= x <= front.x + front.width + 8 and
                              front.y - 40 <= y <= front.y + front.height + 8):
                    continue
                self.click_at(x, y)
                return
        raise RuntimeError("window has no exposed client area for a pointer focus test")


def exercise(guest, screenshot, close_first):
    first = guest.focused()
    first_pid = guest.shell_pid()
    stamp = str(time.monotonic_ns() % 100000000)
    guest.command("clear", re.compile(r"\x1b\[2J"))
    guest.redirected_echo("filefirst" + stamp)
    guest.echo("first" + stamp)
    ready = guest.command("nxshell", re.compile(r"TTY Window userland active \(PID (\d+)\)"))
    second_pid = int(ready[1])
    if second_pid == first_pid:
        raise RuntimeError("new shell reported the first process identity")
    # GUI classification finishes after the shell banner. Each short probe
    # begins with Enter so a focus transition midway through typing cannot
    # leave a partial command in the newly focused shell.
    deadline = time.monotonic() + guest.timeout
    identity = re.compile(rf"pid={second_pid} level=user caps=")
    while time.monotonic() < deadline:
        try:
            guest.command("\nid", identity, timeout=1.5)
            break
        except TimeoutError:
            continue
    else:
        raise RuntimeError("second shell did not acquire focus")
    second = guest.focused()
    if first.id == second.id or first.owner == second.owner:
        raise RuntimeError(f"both shells use the same terminal: {first}, {second}")
    # A focus hint alone does not establish stacking. A click where the two
    # client areas overlap must land on the newly opened shell.
    left = max(first.x, second.x) + 16
    top = max(first.y, second.y) + 40
    right = min(first.x + first.width, second.x + second.width) - 16
    bottom = min(first.y + first.height, second.y + second.height) - 16
    if left <= right and top <= bottom:
        guest.click_at((left + right) // 2, (top + bottom) // 2)
        if guest.shell_pid() != second_pid:
            raise RuntimeError("new terminal had focus behind the previous window")
    guest.command("clear", re.compile(r"\x1b\[2J"))
    guest.echo("second" + stamp)
    time.sleep(0.6)
    guest.screenshot(screenshot)
    guest.click(first, second)
    if guest.shell_pid() != first_pid:
        raise RuntimeError("clicking the first terminal did not select its shell")
    guest.command("clear", re.compile(r"\x1b\[2J"))
    guest.echo("firstagain" + stamp)
    time.sleep(0.6)
    guest.screenshot(screenshot.with_stem(screenshot.stem + "-first"))
    if close_first:
        guest.command("exit", re.compile("Exiting NXShell"))
        nxadb.qmp_type(guest.qmp, "\n")
        time.sleep(1)
        guest.click(second, first)
        deadline = time.monotonic() + guest.timeout
        while time.monotonic() < deadline:
            listing = guest.command("nxwins", re.compile(
                r"ID  PID  STATE.*?(?=\x1b\[32mNXShell)", re.S)).group()
            if not re.search(rf"^{first.id}\s+{first.owner}\s+", listing, re.M):
                break
            time.sleep(0.3)
        else:
            raise RuntimeError("the first TTY window remained after shell exit")
    else:
        guest.click(second, first)
    if guest.shell_pid() != second_pid:
        raise RuntimeError("the second shell no longer receives its own input")
    if close_first:
        guest.command("/sys/bin/nxtest_userkit_nxttyd", re.compile(
            r"\[USERKIT\]\[PASS\] terminal port=\S+ rc=0"))
    guest.redirected_echo("filesecond" + stamp)
    guest.echo("independent" + stamp)
    time.sleep(0.6)
    guest.screenshot(screenshot.with_stem(screenshot.stem + "-second"))
    print(f"PASS first shell {first_pid} / TTY {first.owner} / window {first.id}; "
          f"second shell {second_pid} / TTY {second.owner} / window {second.id}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qmp", type=Path, required=True)
    parser.add_argument("--uart", type=Path, required=True)
    parser.add_argument("--screenshot", type=Path, required=True)
    parser.add_argument("--arch", choices=("amd64", "aarch64"), required=True)
    parser.add_argument("--timeout", type=int, default=15)
    parser.add_argument("--close-first", action="store_true",
                        help="exit the initial shell before testing the other terminal again")
    args = parser.parse_args()
    if b"[NXTEST] done:" not in args.uart.read_bytes():
        parser.error("wait for the guest boot suite to finish and dismiss its terminal")
    exercise(Guest(args.qmp, args.uart, args.timeout), args.screenshot, args.close_first)


if __name__ == "__main__":
    main()
