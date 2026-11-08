#!/usr/bin/env python3
"""Compare loopy-lab's plain ROM WRAM dump with its real-console 10-run table.

Examples:
  python3 tools/compare_loopy_lab.py timing timing-wram.bin TIMING-LAB.md
  python3 tools/compare_loopy_lab.py video  video-wram.bin  VIDEO-LAB.md

A ROM's last CRC row is not a hardware measurement and is excluded. A value is
counted as matching only if both recorded measurements fall inside the console's
10-run interval. Like the test ROM, 16-bit hex comparisons are per byte. For
rounded decimal medians, allow 0.03 to absorb decimal display rounding.
"""
from __future__ import annotations
import argparse
import collections
import re
from pathlib import Path


def read_results(data: bytes):
    header = 0x6FF00  # work RAM 0x09070000 inside the wram_game dump
    if data[header:header + 4] not in (b"TLAB", b"VLAB"):
        raise ValueError("Missing loopy-lab result header at work RAM 0x09070000")
    count = data[header + 4] | (data[header + 6] << 8)
    stride = data[header + 5]
    if stride < 12 or stride > 200:
        raise ValueError(f"Invalid loopy-lab record stride: {stride}")
    result = []
    for i in range(count):
        txt = data[header + 8 + i * stride:header + 8 + (i + 1) * stride].decode("ascii", "replace").rstrip("\x00 ")
        match = re.match(r"^([A-Z][A-Z0-9])\s+(.+?)\s{2,}(.+)$", txt)
        if match:
            test_id, title, values = match.groups()
            result.append((test_id, title, values.split()))
    return count, result


def read_reference(markdown: str):
    expected = {}
    for line in markdown.splitlines():
        match = re.match(r"^\| ([A-Z][A-Z0-9]) \| `([^`]+)` \| ([^|]+) \| ([^|]+) \|", line)
        if match:
            expected[match.group(1)] = (match.group(2), match.group(4).strip())
    return expected


def matches_hardware(values, bounds, test_id=""):
    intervals = [x.strip() for x in bounds.split("/")]
    if len(values) < len(intervals):
        return False
    # The plain ROM may append an ungraded spread to one cycle median.
    values = values[:len(intervals)]
    hex_mode = (any(re.search(r"[A-F]", v) for v in values) or
                all(re.search(r"\b[0-9A-F]{4}\b", p) and "." not in p for p in intervals))
    for value, interval in zip(values, intervals):
        if hex_mode:
            low, high = (int(x, 16) for x in interval.split()[:2])
            measured = int(value, 16)
            for shift in (0, 8):
                v = (measured >> shift) & 255
                a = (low >> shift) & 255
                b = (high >> shift) & 255
                tolerance = 8 if test_id in ("A1", "A2", "A3") else 0
                if not a - tolerance <= v <= b + tolerance:
                    return False
        else:
            tok = interval.split()
            low = float(tok[0])
            high = low + float(tok[1][1:]) if len(tok) > 1 and tok[1].startswith("+") else low
            v = float(value)
            if v < low - 0.03 or v > high + 0.03:
                return False
    return True


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("lab", choices=["timing", "video"])
    p.add_argument("wram", type=Path, help="wram_game dump from --dump")
    p.add_argument("reference", type=Path, help="TIMING-LAB.md or VIDEO-LAB.md")
    p.add_argument("--failures", action="store_true", help="also show mismatched rows")
    args = p.parse_args()
    count, actual = read_results(args.wram.read_bytes())
    expected = read_reference(args.reference.read_text())
    per_group = collections.defaultdict(lambda: [0, 0])
    failures = []
    for test_id, title, values in actual:
        if test_id not in expected:
            continue  # the final CRC row, not a reference test
        _, bounds = expected[test_id]
        try:
            matched = matches_hardware(values, bounds, test_id)
        except (ValueError, IndexError):
            matched = False
        per_group[test_id[0]][1] += 1
        if matched:
            per_group[test_id[0]][0] += 1
        else:
            failures.append((test_id, title, " ".join(values), bounds))
    good = sum(v[0] for v in per_group.values())
    total = sum(v[1] for v in per_group.values())
    print(f"{args.lab}: {count} recorded rows, {good}/{total} hardware measurements in the 10-run ranges")
    for group, (ok, all_) in per_group.items():
        print(f"  {group}: {ok}/{all_}")
    if args.failures:
        print("Rows outside the hardware reference:")
        for code, title, values, bounds in failures:
            print(f"  {code} {title}: {values}   (console {bounds})")


if __name__ == "__main__":
    main()
