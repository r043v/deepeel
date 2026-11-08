#!/bin/sh
# SMS/GG fidelity harness: one TAS (.bk2/log) replayed on the Genesis Plus GX
# libretro core (reference) and the m0boy runtime, per-frame work-RAM diff.
# The core .so stays outside the repository (same rule as the ROMs).
set -eu
if [ "$#" -ne 3 ]; then
    echo "usage: $0 ROM movie.bk2 frames" >&2
    exit 2
fi
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
runtime=${M0BOY_DIR:-"$root/../m0boy"}
so=${DP_GPGX_SO:-"$root/../Genesis-Plus-GX/genesis_plus_gx_libretro.so"}
[ -f "$so" ] || { echo "reference core not found: $so" >&2; exit 1; }
cc -O2 -std=c99 -I"$runtime/targets/gamegear/runtime" -I"$runtime/targets/sg1000/runtime" \
    -I"$runtime/targets/common/runtime" -I"$root/src/platform/sega" \
    "$root/tools/gg_compare_reference.c" \
    "$root/build/libdeepeel_sega.a" "$root/build/libdeepeel_gg.a" "$root/build/libdeepeel_z80.a" \
    -lm -ldl -o "$root/build/gg_compare_reference"
DP_GPGX_SO="$so" "$root/build/gg_compare_reference" "$1" "$2" "$3"
