#!/usr/bin/env python3
"""Compare complete physical-ROM residual lists from deepeel JSON reports."""
import argparse
import json
import sys


def load(path):
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    m = data["map"]
    rom_size = data.get("rom_size", data.get("rom", {}).get("size"))
    rows = m["residual"]
    if m.get("residual_omitted", 0) or len(rows) != m["residual_ranges"]:
        raise ValueError(f"{path}: residual list is truncated")
    banks = {}
    for row in rows:
        b, lo, hi = (row[k] for k in ("bank", "lo", "hi"))
        if not all(isinstance(x, int) and not isinstance(x, bool) for x in (b, lo, hi)):
            raise ValueError(f"{path}: invalid range coordinates")
        if b < 0 or not 0 <= lo <= hi < 0x4000 or row["len"] != hi - lo + 1:
            raise ValueError(f"{path}: invalid residual range")
        if rom_size is not None and b * 0x4000 + hi >= rom_size:
            raise ValueError(f"{path}: range exceeds ROM size")
        banks.setdefault(b, []).append((lo, hi + 1))
    for spans in banks.values():
        spans.sort()
        if any(a[1] > b[0] for a, b in zip(spans, spans[1:])):
            raise ValueError(f"{path}: overlapping residual ranges")
    if sum(hi - lo for spans in banks.values() for lo, hi in spans) != m["residual_bytes"]:
        raise ValueError(f"{path}: residual byte count mismatch")
    return data, banks


def subtract(left, right):
    result = []
    for bank, spans in sorted(left.items()):
        others = right.get(bank, [])
        j = 0
        for lo, hi in spans:
            while j < len(others) and others[j][1] <= lo:
                j += 1
            pos = lo
            k = j
            while k < len(others) and others[k][0] < hi:
                a, b = others[k]
                if a > pos:
                    result.append((bank, pos, min(a, hi)))
                pos = max(pos, b)
                if pos >= hi:
                    break
                k += 1
            if pos < hi:
                result.append((bank, pos, hi))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before")
    parser.add_argument("after")
    args = parser.parse_args()
    try:
        before, a = load(args.before)
        after, b = load(args.after)
        size_a = before.get("rom_size", before.get("rom", {}).get("size"))
        size_b = after.get("rom_size", after.get("rom", {}).get("size"))
        if size_a is not None and size_b is not None and size_a != size_b:
            raise ValueError("ROM sizes differ")
        platform_a = before.get("platform", "gb" if "rom" in before else None)
        platform_b = after.get("platform", "gb" if "rom" in after else None)
        if platform_a and platform_b and platform_a != platform_b:
            raise ValueError("platforms differ")
        # The report schema has no guaranteed ROM hash: identity must be checked
        # externally. Comparing different ROMs would otherwise look meaningful.
        removed, added = subtract(a, b), subtract(b, a)
        print(json.dumps({"identity_verified": False,
                          "warning": "Verify identical ROM and compatible map settings externally; classification is not execution.",
                          "became_classified_bytes": sum(h - l for _, l, h in removed),
                          "became_unknown_bytes": sum(h - l for _, l, h in added),
                          "became_classified": [{"bank": x, "lo": l, "hi": h - 1} for x, l, h in removed],
                          "became_unknown": [{"bank": x, "lo": l, "hi": h - 1} for x, l, h in added]}, indent=2))
    except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError) as e:
        print(f"compare_residual: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
