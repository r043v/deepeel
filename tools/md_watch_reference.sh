#!/bin/sh
# Side-by-side SDL2 view: ClownMDEmu vs m0boy replaying one raw GMV. usage: ROM movie.gmv [frames] [scale]
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
reference=${CLOWN_MD_DIR:-"$root/../clownmdemu-core"}
runtime=${M0BOY_DIR:-"$root/../m0boy"}
ref_build=${DP_MD_REF_BUILD:-"$root/build/md-reference"}
cmake -S "$reference" -B "$ref_build" -DCC_USE_C99_INTEGERS=ON >&2
cmake --build "$ref_build" -j 4 >&2
cc -O2 -std=c99 -DCC_USE_C99_INTEGERS $(pkg-config --cflags sdl2) \
    -I"$reference/source" -I"$runtime/targets/megadrive/runtime" -I"$runtime/targets/sg1000/runtime" \
    "$root/tools/md_watch_reference.c" "$ref_build/libclownmdemu-core.a" \
    "$ref_build/libraries/clown68000/libclown68000-interpreter.a" "$ref_build/libraries/clown68000/libclown68000-common.a" \
    "$ref_build/libraries/clownz80/libclownz80-interpreter.a" "$ref_build/libraries/clownz80/libclownz80-common.a" \
    "$root/build/libdp_mdmach_lib.a" -lm $(pkg-config --libs sdl2) -o "$ref_build/md_watch_reference"
rom=$1; shift; mov=$1; shift
exec "$ref_build/md_watch_reference" "$rom" "$mov" "$@"
