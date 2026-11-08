#!/bin/sh
# PCE fidelity harness: one TAS (.bk2 or plain text) replayed on the Beetle
# PCE libretro core (Mednafen reference) and the m0boy PCE runtime, per-frame
# 8 KiB work-RAM diff.  The core .so stays outside the repository.
set -eu
if [ "$#" -ne 3 ]; then
    echo "usage: $0 ROM movie frames" >&2
    exit 2
fi
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
runtime=${M0BOY_DIR:-"$root/../m0boy"}
so=${DP_BEETLE_SO:-"$root/../beetle-pce-libretro/mednafen_pce_libretro.so"}
[ -f "$so" ] || { echo "reference core not found: $so" >&2; exit 1; }
cc -O2 -std=c99 -I"$root/src/platform/pce" -I"$runtime/targets/pce/runtime" \
    "$root/tools/pce_compare_reference.c" \
    "$root/build/libdeepeel_pce.a" "$root/build/libdeepeel_core.a" \
    -lm -ldl -lz -o "$root/build/pce_compare_reference"
DP_BEETLE_SO="$so" "$root/build/pce_compare_reference" "$1" "$2" "$3"
