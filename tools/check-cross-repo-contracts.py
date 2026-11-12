#!/usr/bin/env python3
"""Verify that NX-CORE and NX-SDK expose the same shared wire contracts."""
from pathlib import Path
import json

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "kernel"
SDK = ROOT / "system"

PAIRS = {
    "caps.h": ("include/abi/caps.h", "include/contract/caps.h"),
    "font.h": ("include/abi/font.h", "include/contract/font.h"),
    "nx_abi.h": ("include/abi/nx_abi.h", "include/contract/nx_abi.h"),
    "nxpax.h": ("include/abi/nxpax.h", "include/contract/nxpax.h"),
    "object.h": ("include/abi/object.h", "include/contract/object.h"),
    "os1_compositor.h": ("include/abi/os1_compositor.h", "include/contract/os1_compositor.h"),
    "os1_input.h": ("include/abi/os1_input.h", "include/api/os1_input.h"),
    "os1_time.h": ("include/abi/os1_time.h", "include/contract/os1_time.h"),
    "posix_types.h": ("include/abi/posix_types.h", "include/contract/posix_types.h"),
    "style_names.h": ("include/abi/style_names.h", "include/contract/style_names.h"),
    "surface.h": ("include/abi/surface.h", "include/contract/surface.h"),
    "syscall_nums.h": ("include/abi/syscall_nums.h", "include/contract/syscall_nums.h"),
    "sysstats.h": ("include/abi/sysstats.h", "include/contract/sysstats.h"),
}
SUPPORT_PAIRS = {
    "elf.h": ("include/api/elf.h", "include/api/elf.h"),
    "stdbool.h": ("include/api/stdbool.h", "include/api/stdbool.h"),
}


def version(module: Path, scope: str) -> int:
    data = json.loads((module / "contracts/version.json").read_text())
    value = data["scopes"][scope]["revision"]
    if not isinstance(value, int) or value < 1:
        raise SystemExit(f"{module.name}: invalid {scope} revision")
    return value


def compare(label: str, core_rel: str, sdk_rel: str) -> None:
    left = CORE / core_rel
    right = SDK / sdk_rel
    if not left.is_file() or not right.is_file():
        raise SystemExit(f"{label}: missing pair {left} / {right}")
    if left.read_bytes() != right.read_bytes():
        raise SystemExit(f"{label}: NX-CORE/NX-SDK contract drift")


def main() -> None:
    core_abi = {p.name for p in (CORE / "include/abi").glob("*.h")}
    mapped = set(PAIRS)
    if core_abi != mapped:
        missing = sorted(core_abi - mapped)
        stale = sorted(mapped - core_abi)
        raise SystemExit(
            "cross-contract map incomplete"
            + (f"; unmapped kernel ABI: {', '.join(missing)}" if missing else "")
            + (f"; stale map: {', '.join(stale)}" if stale else "")
        )
    for label, pair in PAIRS.items():
        compare(label, *pair)
    for label, pair in SUPPORT_PAIRS.items():
        compare(label, *pair)
    core_rev = version(CORE, "abi")
    sdk_rev = version(SDK, "abi")
    print(
        f"cross-repository contracts: PASS "
        f"(NX-CORE abi=r{core_rev}, NX-SDK abi=r{sdk_rev}, {len(PAIRS)} wire pairs)"
    )


if __name__ == "__main__":
    main()
