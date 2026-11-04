#!/bin/sh
# Builds tools/gambatte_lr against a patched gambatte-core.
# usage: tools/gambatte_lr_build.sh OUTDIR   (clones pokemon-speedrunning/gambatte-core into OUTDIR/core if absent)
set -e
O=${1:?outdir}; R=$(cd "$(dirname "$0")/.." && pwd); B=${DEEPEEL_BUILD:-$R/build}
mkdir -p "$O/obj"
[ -d "$O/core" ] || { git clone -q --depth 1 https://github.com/pokemon-speedrunning/gambatte-core.git "$O/core"; (cd "$O/core" && git apply "$R/tools/gambatte_trace.patch"); }
G=$O/core/libgambatte
cd "$G"
for f in $(find src -name '*.cpp' | grep -v 'unzip\|cinterface\|file_zip'); do
  inc=; case $f in *snes_spc*) inc="-include cstdint";; esac
  echo "g++ -std=c++17 -O1 -w $inc -Iinclude -Isrc -I../common -c $f -o $O/obj/$(echo $f | tr / _).o"
done | xargs -P"$(nproc)" -I{} sh -c "{}"
g++ -std=c++17 -O1 -w -include cstdint -I"$R/src" -Iinclude -c "$R/tools/gambatte_lr.cpp" -o "$O/obj/zz_lr.o"
g++ "$O"/obj/*.o "$B/libdeepeel_libretro.a" "$B/libdeepeel_core.a" -ldl -lm -lz -o "$O/gambatte_lr"
echo "built $O/gambatte_lr"
