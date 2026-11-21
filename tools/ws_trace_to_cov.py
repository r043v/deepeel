#!/usr/bin/env python3
"""Convert m0boy ws_host --exec-trace output to deepeel's reference .cov format."""

import argparse
import csv
from collections import Counter
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path, help="WonderSwan ROM image used for the trace")
    parser.add_argument("trace", type=Path, help="ws_host --exec-trace CSV")
    parser.add_argument("output", type=Path, help="deepeel .cov output")
    args = parser.parse_args()

    rom_size = args.rom.stat().st_size
    hits: Counter[int] = Counter()
    with args.trace.open(newline="", encoding="ascii") as source:
        rows = csv.DictReader(source)
        if rows.fieldnames != ["pc", "rom_offset"]:
            parser.error("expected ws_host --exec-trace columns pc,rom_offset")
        for line_number, row in enumerate(rows, 2):
            try:
                offset = int(row["rom_offset"], 16)
            except (TypeError, ValueError):
                parser.error(f"invalid ROM offset on trace line {line_number}")
            if not 0 <= offset < rom_size:
                parser.error(f"ROM offset outside the selected image on line {line_number}")
            hits[offset] += 1

    if not hits:
        parser.error("trace contains no cartridge ROM instruction fetches")

    with args.output.open("w", encoding="ascii", newline="\n") as output:
        output.write("# m0boy WonderSwan execution trace, source=played-TAS\n")
        for offset, count in sorted(hits.items()):
            output.write(f"{offset >> 14:02X} {offset & 0x3FFF:04X} 1 {count}\n")
    print(f"{len(hits)} executed ROM bytes, {sum(hits.values())} instruction fetches")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
