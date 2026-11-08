#!/bin/sh
# NES fidelity harness: one TAS (.bk2/.fm2) replayed on the Nestopia libretro
# core (reference) and the m0boy NES runtime, per-frame 2 KiB RAM diff.
# The core .so stays outside the repository (same rule as the ROMs).
set -eu
if [ "$#" -ne 3 ]; then
    echo "usage: $0 ROM movie.bk2|fm2 frames" >&2
    exit 2
fi
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
runtime=${M0BOY_DIR:-"$root/../m0boy"}
so=${DP_NESTOPIA_SO:-"$root/../nestopia-libretro/libretro/nestopia_libretro.so"}
[ -f "$so" ] || { echo "reference core not found: $so" >&2; exit 1; }
cc -O2 -std=c99 -I"$runtime/targets/nes/runtime" -I"$root/src/mach" -I"$root/src/platform/nes" -I"$root/src" \
    "$root/tools/nes_compare_reference.c" \
    "$root/build/libdeepeel_nes.a" "$root/build/libdeepeel_core.a" \
    -lm -ldl -lz -o "$root/build/nes_compare_reference"
DP_NESTOPIA_SO="$so" "$root/build/nes_compare_reference" "$1" "$2" "$3"
