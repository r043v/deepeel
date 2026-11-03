#!/bin/sh
# Build a separate reference executable; never copy reference implementation code.
set -eu
if [ "$#" -ne 3 ]; then
    echo "usage: $0 ROM movie.gmv-or-zip frames" >&2
    exit 2
fi
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
reference=${CLOWN_MD_DIR:-"$root/../clownmdemu-core"}
runtime=${M0BOY_DIR:-"$root/../m0boy"}
ref_build=${DP_MD_REF_BUILD:-"$root/build/md-reference"}
clock_flag=
if [ "${DP_MD_REF_GENS_CLOCK:-0}" = 1 ]; then
    if ! rg -q 'DEEPEEL_GENS_60' "$reference/source/bus-common.c"; then
        echo "reference core lacks the optional DEEPEEL_GENS_60 clock hook" >&2
        exit 1
    fi
    ref_build=${DP_MD_REF_BUILD:-"$root/build/md-reference-gens-clock"}
    clock_flag=-DDEEPEEL_GENS_60
fi
cmake -S "$reference" -B "$ref_build" -DCC_USE_C99_INTEGERS=ON -DCMAKE_C_FLAGS="$clock_flag" >&2
cmake --build "$ref_build" -j 4 >&2
cc -O2 -std=c99 -DCC_USE_C99_INTEGERS \
    -I"$reference/source" -I"$runtime/targets/megadrive/runtime" -I"$runtime/targets/sg1000/runtime" \
    "$root/tools/md_compare_reference.c" "$ref_build/libclownmdemu-core.a" \
    "$ref_build/libraries/clown68000/libclown68000-interpreter.a" \
    "$ref_build/libraries/clown68000/libclown68000-common.a" \
    "$ref_build/libraries/clownz80/libclownz80-interpreter.a" \
    "$ref_build/libraries/clownz80/libclownz80-common.a" \
    "$root/build/libdp_mdmach_lib.a" -lm -o "$ref_build/md_compare_reference"
case "$2" in
    *.zip|*.ZIP)
        tmp=$(mktemp "${TMPDIR:-/tmp}/deepeel-gmv.XXXXXXXX")
        trap 'rm -f "$tmp"' EXIT HUP INT TERM
        unzip -p "$2" '*.gmv' > "$tmp"
        "$ref_build/md_compare_reference" "$1" "$tmp" "$3"
        ;;
    *) "$ref_build/md_compare_reference" "$1" "$2" "$3" ;;
esac
