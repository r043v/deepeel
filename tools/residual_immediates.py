#!/usr/bin/env python3
"""Find unproven SM83 immediate-address candidates into unknown ROM bytes."""
import argparse
import hashlib
import json
import sys

OPS = {0x01: "ld bc,nn", 0x11: "ld de,nn", 0x21: "ld hl,nn"}
SPLIT_OPS = {0x06: ("b", "bc", "high"), 0x0e: ("c", "bc", "low"),
             0x16: ("d", "de", "high"), 0x1e: ("e", "de", "low"),
             0x26: ("h", "hl", "high"), 0x2e: ("l", "hl", "low")}


def scan(rom, classes, limit, starts=None):
    hits, total = [], 0
    banks = (len(rom) + 0x3fff) // 0x4000
    for site in range(max(0, len(rom) - 2)):
        op = rom[site]
        if op not in OPS or site // 0x4000 != (site + 2) // 0x4000:
            continue
        if starts is not None and site not in starts:
            continue
        # The map labels instruction bytes, not starts: even this pattern may
        # start inside another instruction. Never label the result as proven.
        if classes[site:site + 3] not in ("111", "222"):
            continue
        addr = rom[site + 1] | rom[site + 2] << 8
        if addr >= 0x8000:
            continue
        for bank in range(banks):
            target = bank * 0x4000 + (addr & 0x3fff)
            if target >= len(rom) or classes[target] != "0":
                continue
            total += 1
            if len(hits) < limit:
                hits.append({"site_index": site, "site_class": "natural" if classes[site] == "1" else "forced",
                             "pattern": OPS[op], "cpu_address": addr, "candidate_bank": bank,
                             "target_index": target, "observed_start": starts is not None})
    return hits, total


def scan_split(rom, classes, limit, starts):
    """Propose nearby immediate loads; proximity does not establish data flow."""
    sites = sorted(i for i in starts if i + 1 < len(rom)
                   and i // 0x4000 == (i + 1) // 0x4000
                   and rom[i] in SPLIT_OPS and classes[i:i + 2] in ("11", "22"))
    hits, total = [], 0
    banks = (len(rom) + 0x3fff) // 0x4000
    for n, first in enumerate(sites):
        _, pair, half = SPLIT_OPS[rom[first]]
        for j in range(n + 1, len(sites)):
            second = sites[j]
            if second - first > 8 or first // 0x4000 != second // 0x4000:
                break
            _, other_pair, other_half = SPLIT_OPS[rom[second]]
            if pair != other_pair or half == other_half:
                continue
            high, low = (first, second) if half == "high" else (second, first)
            addr = rom[high + 1] << 8 | rom[low + 1]
            if addr >= 0x8000:
                continue
            for bank in range(banks):
                target = bank * 0x4000 + (addr & 0x3fff)
                if target >= len(rom) or classes[target] != "0":
                    continue
                total += 1
                if len(hits) < limit:
                    hits.append({"high_site_index": high, "low_site_index": low,
                                 "high_site_class": "natural" if classes[high] == "1" else "forced",
                                 "low_site_class": "natural" if classes[low] == "1" else "forced",
                                 "pattern": "split ld " + pair, "cpu_address": addr,
                                 "candidate_bank": bank, "target_index": target})
    return hits, total


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("rom")
    p.add_argument("map", help="GB kernel text map from --map (not PNG)")
    p.add_argument("--summary", help="kernel JSON summary from the same run, to restrict sites to observed starts")
    p.add_argument("--limit", type=int, default=10000)
    p.add_argument("--split-immediates", action="store_true",
                   help="also propose nearby 8-bit register loads (requires --summary)")
    args = p.parse_args()
    try:
        if args.limit < 0:
            raise ValueError("limit must be non-negative")
        if args.split_immediates and not args.summary:
            raise ValueError("--split-immediates requires --summary")
        with open(args.rom, "rb") as f:
            rom = f.read()
        with open(args.map, encoding="ascii") as f:
            lines = f.read().splitlines()
        if len(lines) != 2 or lines[0] != "gb map v1" or len(lines[1]) != len(rom):
            raise ValueError("expected a GB kernel text map with the same length as the ROM")
        classes = lines[1]
        if any(not 0 <= ord(c) - ord("0") < 14 for c in classes):
            raise ValueError("invalid map class")
        starts = None
        omitted = None
        if args.summary:
            with open(args.summary, encoding="utf-8") as f:
                summary = json.load(f)
            if summary.get("platform") != "gb" or summary.get("engine") != "kernel" or summary.get("rom_size") != len(rom):
                raise ValueError("summary must be a GB kernel report for the same ROM size")
            rows = summary["instruction_starts"]
            omitted = summary["instruction_starts_omitted"]
            if len(rows) + omitted != summary["instruction_starts_total"]:
                raise ValueError("inconsistent instruction start list")
            starts = set()
            for row in rows:
                ix = row["index"]
                if not isinstance(ix, int) or isinstance(ix, bool) or not 0 <= ix < len(rom) or ix in starts:
                    raise ValueError("invalid instruction start index")
                starts.add(ix)
        hits, total = scan(rom, classes, args.limit, starts)
        if args.split_immediates:
            split_hits, split_total = scan_split(rom, classes, max(0, args.limit - len(hits)), starts)
            hits.extend(split_hits)
            total += split_total
        print(json.dumps({"rom_sha256": hashlib.sha256(rom).hexdigest(),
                          "warning": "Hypotheses only: bank mapping, execution order and data flow are not proven; verify that map and summary match ROM and run.",
                          "starts_filter": starts is not None, "starts_omitted": omitted,
                          "split_immediates": args.split_immediates,
                          "candidates_total": total, "candidates_omitted": total - len(hits),
                          "candidates": hits}, indent=2))
    except (OSError, UnicodeError, ValueError, KeyError, TypeError) as e:
        print(f"residual_immediates: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
