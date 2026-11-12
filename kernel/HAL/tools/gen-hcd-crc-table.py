#!/usr/bin/env python3
"""Generate the checked-in CRC-64/ECMA-182 lookup table deterministically."""

POLYNOMIAL = 0x42F0E1EBA9EA3693
MASK64 = (1 << 64) - 1


def table_entry(index: int) -> int:
    value = index << 56
    for _ in range(8):
        if value & (1 << 63):
            value = ((value << 1) ^ POLYNOMIAL) & MASK64
        else:
            value = (value << 1) & MASK64
    return value


def main() -> None:
    values = [table_entry(index) for index in range(256)]
    lines = []
    for start in range(0, 256, 4):
        group = values[start:start + 4]
        line = ", ".join(f"UINT64_C(0x{value:016X})" for value in group)
        if start + 4 < 256:
            line += ","
        lines.append(line)
    print("\n".join(lines))


if __name__ == "__main__":
    main()
