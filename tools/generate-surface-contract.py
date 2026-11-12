#!/usr/bin/env python3
"""Generate independent NX-CORE and NX-SDK headers from the wire specification."""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def render(spec):
    lines = ["/* Generated from kernel/contracts/surface-v1.json. Do not edit. */",
             "#ifndef NX_SURFACE_ABI_H", "#define NX_SURFACE_ABI_H",
             "#include <stdint.h>", "#include <stddef.h>", "#include <os1_input.h>"]
    for key, value in spec["constants"].items():
        lines.append(f"#define {key} {value}")
    for item in spec["structs"]:
        lines.append(f"struct {item['name']} {{")
        for kind, name, _ in item["fields"]:
            lines.append(f"  {kind} {name};")
        lines.append("};")
        lines.append("#if defined(__cplusplus)")
        lines.append("#define NX_SURFACE_ASSERT static_assert")
        lines.append("#else")
        lines.append("#define NX_SURFACE_ASSERT _Static_assert")
        lines.append("#endif")
        lines.append(f'NX_SURFACE_ASSERT(sizeof(struct {item["name"]}) == {item["size"]}, "surface ABI size");')
        for _, name, offset in item["fields"]:
            lines.append(f'NX_SURFACE_ASSERT(offsetof(struct {item["name"]}, {name}) == {offset}, "surface ABI offset");')
        lines.append("#undef NX_SURFACE_ASSERT")
    return "\n".join(lines + ["#endif", ""])

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    content = render(json.loads((ROOT / "kernel/contracts/surface-v1.json").read_text()))
    for target in ("kernel/include/abi/surface.h", "system/include/contract/surface.h"):
        path = ROOT / target
        if args.check:
            if not path.exists() or path.read_text() != content:
                raise SystemExit(f"stale generated contract: {target}")
        else:
            path.write_text(content)

if __name__ == "__main__":
    main()
