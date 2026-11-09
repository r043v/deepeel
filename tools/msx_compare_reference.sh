#!/bin/sh
# MSX fidelity harness: one TAS (.bk2/plain input log) replayed on the fMSX
# libretro core (reference) and the m0boy MSX runtime, per-frame work-RAM diff.
# The core .so stays outside the repository (same rule as the ROMs); fMSX also
# wants its BIOS files in the system directory (DP_MSX_SYS, default /tmp).
set -eu
if [ "$#" -ne 3 ]; then
    echo "usage: $0 ROM movie.frames count" >&2
    exit 2
fi
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
runtime=${M0BOY_DIR:-"$root/../m0boy"}
so=${DP_FMSX_SO:-"$root/../lr-cores/fmsx_libretro.so"}
[ -f "$so" ] || { echo "reference core not found: $so" >&2; exit 1; }
cc -O2 -std=c99 -DSG_MSX=1 -DSG_MSX_RAM=65536 -I"$runtime/targets/msx/runtime" -I"$runtime/targets/sg1000/runtime" \
    -I"$runtime/targets/common/runtime" -I"$root/src/platform/sega" -I"$root/src/platform/msx" \
    "$root/tools/msx_compare_reference.c" \
    "$root/build/libdeepeel_msx.a" "$root/build/libdeepeel_sega.a" "$root/build/libdeepeel_z80.a" \
    -lm -ldl -o "$root/build/msx_compare_reference"
DP_FMSX_SO="$so" "$root/build/msx_compare_reference" "$1" "$2" "$3"
