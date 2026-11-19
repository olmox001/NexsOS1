#!/usr/bin/env python3
"""Generate independent NX-CORE and NX-SDK headers from the wire specification."""
import argparse
import ctypes
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
    return "\n".join(lines + ["#endif", ""])

def validate_layout(spec):
    """Fail generator/check on an incorrect wire layout without adding
    forbidden static assertions or conditional code to production headers.
    All surface fields have stable fixed-width C types on both target ABIs.
    """
    class RawInput(ctypes.Structure):
        _fields_ = [
            ("abi_version", ctypes.c_uint32), ("struct_size", ctypes.c_uint32),
            ("sequence", ctypes.c_uint64), ("producer_cpu", ctypes.c_uint32),
            ("provider", ctypes.c_uint8), ("device_id", ctypes.c_uint8),
            ("reserved", ctypes.c_uint8 * 2), ("type", ctypes.c_uint16),
            ("code", ctypes.c_uint16), ("value", ctypes.c_int32)]
    types = {"uint32_t": ctypes.c_uint32, "uint64_t": ctypes.c_uint64,
             "int32_t": ctypes.c_int32,
             "struct os1_raw_input_event_v1": RawInput}
    for item in spec["structs"]:
        fields = [(name, types[kind]) for kind, name, _ in item["fields"]]
        model = type(item["name"], (ctypes.Structure,), {"_fields_": fields})
        if ctypes.sizeof(model) != item["size"]:
            raise ValueError(f'{item["name"]}: expected size {item["size"]}, '
                             f'actual {ctypes.sizeof(model)}')
        for _, name, offset in item["fields"]:
            actual = getattr(model, name).offset
            if actual != offset:
                raise ValueError(f'{item["name"]}.{name}: expected {offset}, '
                                 f'actual {actual}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    spec = json.loads((ROOT / "kernel/contracts/surface-v1.json").read_text())
    validate_layout(spec)
    content = render(spec)
    for target in ("kernel/include/abi/surface.h", "system/include/contract/surface.h"):
        path = ROOT / target
        if args.check:
            if not path.exists() or path.read_text() != content:
                raise SystemExit(f"stale generated contract: {target}")
        else:
            path.write_text(content)

if __name__ == "__main__":
    main()
