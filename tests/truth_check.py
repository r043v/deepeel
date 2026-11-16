#!/usr/bin/env python3
# truth_check.py — ground-truth accounting of a memory map ("zero data marked code",
# plan point 1).  Compares a "map v1" text (the kernel's class vector) with a truth
# file that states, per byte range, whether the ROM holds code or data.
#
# truth file: lines "<lo>-<hi> c|d" (hex offsets, inclusive; '#' comments), the
# ranges not listed count as neither.
#
# Counts, over the bytes the truth states:
#   code found / missed code   (truth c; proven code = 1 natural, 3 static; the
#                               unproven claims 2 forced, 10 inferred-code,
#                               12 probable-code also count as found)
#   data-as-code proven        (truth d, class 1/3 — an error, the exit criterion;
#                               a forced mark is a fabricated execution, not a claim
#                               the kernel proves, so it stays unproven)
#   data-as-code unproven      (truth d, class 2/10/12 — reported to be judged,
#                               not fatal)
#
# usage: truth_check.py MAP_FILE TRUTH_FILE
# exit 0 when no proven data-as-code error, 1 otherwise (bad inputs: exit 2)
import sys

CODE = set("13")            # proven code (map v1 digits)
CODE_UNPROVEN = set("2:<")  # 2 forced, 10 inferred-code, 12 probable-code


def classes(path):
    with open(path) as f:
        if not f.readline().rstrip().endswith("map v1"):
            sys.exit(f"{path}: not a map v1 text")
        line = f.readline().rstrip("\n")
    return line


def truth(path):
    spans = []   # (lo, hi, 'c'|'d')
    with open(path) as f:
        for ln in f:
            ln = ln.strip()
            if not ln or ln.startswith("#"):
                continue
            parts = ln.split()
            if len(parts) != 2 or parts[1] not in ("c", "d") or "-" not in parts[0]:
                sys.exit(f"{path}: bad line: {ln}")
            lo, hi = parts[0].split("-")
            spans.append((int(lo, 16), int(hi, 16), parts[1]))
    return spans


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    cls = classes(sys.argv[1])
    spans = truth(sys.argv[2])
    found = missed = err = unprov = total_c = total_d = 0
    for lo, hi, kind in spans:
        for i in range(lo, min(hi + 1, len(cls))):
            c = cls[i]
            if kind == "c":
                total_c += 1
                if c in CODE or c in CODE_UNPROVEN:
                    found += 1
                else:
                    missed += 1
            else:
                total_d += 1
                if c in CODE:
                    err += 1
                elif c in CODE_UNPROVEN:
                    unprov += 1
    name = sys.argv[1].rsplit("/", 1)[-1]
    print(f"{name}: code found {found}/{total_c} missed {missed} | "
          f"data-as-code proven {err} unproven {unprov} (over {total_d} data bytes)")
    sys.exit(1 if err else 0)


if __name__ == "__main__":
    main()
