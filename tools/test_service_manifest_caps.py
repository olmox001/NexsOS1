#!/usr/bin/env python3
"""Compile and exercise the real USERKIT manifest authority parser."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "user/USERKIT/sbin/common/nxinit_runtime.c"
MANIFESTS = (
    ROOT / "user/NexsOS1-main/sys/boot/services.conf",
    ROOT / "user/demo1/sys/boot/services.conf",
)


def extract(source: str, name: str) -> str:
    prefix = "static int " + name + "("
    start = source.find(prefix)
    if start < 0 or source.find(prefix, start + 1) >= 0:
        raise RuntimeError("source parser was not found uniquely: " + name)
    brace = source.find("{", start)
    if brace < 0:
        raise RuntimeError("missing parser body: " + name)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        if source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise RuntimeError("unbalanced parser: " + name)


def main() -> None:
    source = SOURCE.read_text()
    bodies = "\n\n".join(extract(source, name) for name in
                         ("nxinit_cap_token", "nxinit_caps_from_manifest"))
    tests = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <caps.h>
TESTED_SOURCE
static int verify(const char *text, int accepted, uint32_t expected) {
  uint32_t result = 0x55aa55aaU;
  int rc = nxinit_caps_from_manifest(text, &result);
  if (accepted && (rc != 0 || result != expected)) return 1;
  if (!accepted && (rc != -EINVAL || result != 0x55aa55aaU)) return 2;
  return 0;
}
int main(int argc, char **argv) {
  if (verify("caps:none", 1, 0U) != 0) return 1;
  if (verify("caps:spawn,fs-write,ipc-any,window,registry-write",
             1, CAP_ALL) != 0) return 2;
  if (verify("caps:window-manage", 1, CAP_WINDOW_MANAGE) != 0) return 3;
  if (verify("caps:keyboard-raw", 1, CAP_KEYBOARD_RAW) != 0) return 17;
  if (verify("caps:window,pointer-raw", 1,
             CAP_WINDOW | CAP_POINTER_RAW) != 0) return 18;
  if (verify("caps:keyboard-raw,keyboard-raw", 0, 0U) != 0) return 19;
  if (verify("caps:window,ipc-any", 1, CAP_WINDOW | CAP_IPC_ANY) != 0)
    return 4;
  if (verify("caps:unknown", 0, 0U) != 0) return 5;
  if (verify("caps:", 0, 0U) != 0) return 6;
  if (verify("window-manage", 0, 0U) != 0) return 7;
  if (verify("caps:window,window", 0, 0U) != 0) return 8;
  if (verify("caps:window,", 0, 0U) != 0) return 9;
  if (verify("caps:,window", 0, 0U) != 0) return 10;
  if (verify("caps:none,window", 0, 0U) != 0) return 11;
  if (verify("caps:window,none", 0, 0U) != 0) return 12;
  if (verify("caps:window-manager", 0, 0U) != 0) return 13;
  if (verify("caps:spawn,fs-write,ipc-any,window,registry-write,"
             "window-manage,spawn", 0, 0U) != 0) return 14;
  for (int i = 1; i < argc; ++i) {
    uint32_t mask = 0U;
    if (nxinit_caps_from_manifest(argv[i], &mask) != 0) return 15;
    if ((mask & ~(CAP_ALL | CAP_WINDOW_MANAGE | CAP_KEYBOARD_RAW | CAP_POINTER_RAW)) != 0U)
      return 16;
  }
  puts("PASS: real NXINIT manifest capability parser and negative cases");
  return 0;
}
'''
    tests = tests.replace("TESTED_SOURCE", bodies)
    args = []
    for manifest in MANIFESTS:
        lines = manifest.read_text().splitlines()
        entries = 0
        for line in lines:
            if not line or line.startswith("#"):
                continue
            fields = line.split("|")
            if len(fields) != 6:
                raise RuntimeError("incomplete authority declaration: " + line)
            args.append(fields[5])
            rights = set(fields[5][5:].split(','))
            if fields[0] in ('keyboard', 'pointer'):
                if rights & {'keyboard-raw', 'pointer-raw'} != {fields[0] + '-raw'}:
                    raise RuntimeError('input router has no raw input authority: ' + line)
                if fields[0] == 'keyboard' and 'window' in rights:
                    raise RuntimeError('keyboard router has unnecessary graphics authority')
            elif rights & {'keyboard-raw', 'pointer-raw'}:
                raise RuntimeError('ordinary service has raw input authority: ' + line)
            entries += 1
        print(manifest.relative_to(ROOT), "manifest entries:", entries)
    with tempfile.TemporaryDirectory(prefix="s4-manifest-caps-") as work:
        fixture = Path(work) / "test.c"
        binary = Path(work) / "test"
        fixture.write_text(tests)
        subprocess.run(["clang", "-std=c11", "-O1", "-g", "-Wall",
                        "-Wextra", "-Werror", "-fsanitize=address,undefined",
                        "-I" + str(ROOT / "system/include/contract"),
                        str(fixture), "-o", str(binary)], check=True)
        subprocess.run([str(binary), *args], check=True)


if __name__ == "__main__":
    main()
