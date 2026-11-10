#!/usr/bin/env python3
"""Verify TIME wire-structure layout for both NexsOS target ABIs without C assertions."""
from __future__ import annotations

import json
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = ROOT / "kernel/contracts/time-v1.json"
HEADER = ROOT / "kernel/include/abi/os1_time.h"
TARGETS = (
    ("amd64", "x86_64-unknown-none-elf"),
    ("aarch64", "aarch64-unknown-none-elf"),
)

STRUCT_LINE = re.compile(r"^\s*(\d+) \| struct ([A-Za-z_][A-Za-z0-9_]*)\s*$")
FIELD_LINE = re.compile(r"^\s*(\d+) \|\s+.+\s([A-Za-z_][A-Za-z0-9_]*)\s*$")
SIZE_LINE = re.compile(r"^\s*\| \[sizeof=(\d+), align=(\d+)\]\s*$")


def collect_layouts(output: str) -> dict[str, dict]:
    layouts: dict[str, dict] = {}
    current: dict | None = None
    for raw in output.splitlines():
        struct_match = STRUCT_LINE.match(raw)
        if struct_match:
            current = {
                "size": None,
                "align": None,
                "fields": {},
            }
            layouts[struct_match.group(2)] = current
            continue
        if current is None:
            continue
        size_match = SIZE_LINE.match(raw)
        if size_match:
            current["size"] = int(size_match.group(1))
            current["align"] = int(size_match.group(2))
            current = None
            continue
        field_match = FIELD_LINE.match(raw)
        if field_match:
            current["fields"][field_match.group(2)] = int(field_match.group(1))
    return layouts


def dump_layout(target: str) -> tuple[dict[str, dict] | None, str]:
    with tempfile.TemporaryDirectory(prefix="nexs-time-layout-") as temp_dir:
        source = Path(temp_dir) / "probe.c"
        source.write_text(
            '#include "os1_time.h"\n'
            "struct os1_time_cpu_v1 probe_time_cpu;\n"
            "struct os1_time_snapshot_v1 probe_time_snapshot;\n"
            "struct os1_timer_cpu_stats_v4 probe_timer_cpu;\n"
            "struct os1_timer_stats_snapshot_v4 probe_timer_snapshot;\n"
        )
        cmd = [
            "clang",
            f"--target={target}",
            "-std=gnu17",
            "-ffreestanding",
            "-I",
            str(HEADER.parent),
            "-Xclang",
            "-fdump-record-layouts-complete",
            "-fsyntax-only",
            str(source),
        ]
        run = subprocess.run(
            cmd,
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
            timeout=30,
        )
        output = run.stdout + run.stderr
        if run.returncode != 0:
            return None, output
        return collect_layouts(output), ""


def verify_arch(arch: str, target: str, spec: dict) -> list[str]:
    layouts, error = dump_layout(target)
    if layouts is None:
        return [f"{arch}: clang layout probe failed: {error.strip()}"]
    failures: list[str] = []
    for item in spec["structs"]:
        name = item["name"]
        actual = layouts.get(name)
        if actual is None:
            failures.append(f"{arch}: missing layout for struct {name}")
            continue
        if actual["size"] != int(item["size"]):
            failures.append(
                f"{arch}: struct {name} size={actual['size']} expected={item['size']}"
            )
        for field in item["fields"]:
            member = field["member"]
            actual_offset = actual["fields"].get(member)
            if actual_offset != int(field["offset"]):
                failures.append(
                    f"{arch}: struct {name}.{member} offset={actual_offset} "
                    f"expected={field['offset']}"
                )
    return failures


def main() -> int:
    spec = json.loads(SPEC.read_text())
    failures: list[str] = []
    for arch, target in TARGETS:
        failures.extend(verify_arch(arch, target, spec))
    if failures:
        print("TIME contract layout: FAIL")
        for item in failures:
            print("  " + item)
        return 1
    print("TIME contract layout: PASS (amd64, aarch64)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
