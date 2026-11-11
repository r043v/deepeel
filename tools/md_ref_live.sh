#!/bin/sh
# Build the live reference tile harness (tools/md_ref_live) against the
# external ClownMDEmu library, next to the differential bench (md_compare_reference).
# Never copies reference implementation code; the harness runs out of tree.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
reference=${CLOWN_MD_DIR:-"$root/../clownmdemu-core"}
ref_build=${DP_MD_REF_BUILD:-"$root/build/md-reference"}
cmake -S "$reference" -B "$ref_build" -DCC_USE_C99_INTEGERS=ON >&2
cmake --build "$ref_build" -j 4 >&2
cc -O2 -std=gnu99 -DCC_USE_C99_INTEGERS \
    -I"$reference/source" -I"$root/src" \
    "$root/tools/md_ref_live.c" \
    "$root/src/platform/md/movie.c" "$root/src/core/zip.c" "$root/src/core/bk2.c" \
    "$root/src/core/sha1.c" "$root/src/core/md5.c" "$root/src/core/bizhawk.c" \
    "$ref_build/libclownmdemu-core.a" \
    "$ref_build/libraries/clown68000/libclown68000-interpreter.a" \
    "$ref_build/libraries/clown68000/libclown68000-common.a" \
    "$ref_build/libraries/clownz80/libclownz80-interpreter.a" \
    "$ref_build/libraries/clownz80/libclownz80-common.a" \
    -lz -lm -o "$ref_build/md_ref_live"
echo "$ref_build/md_ref_live"
