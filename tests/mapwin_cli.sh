#!/bin/sh
# mapwin_cli.sh DEEPEEL ROM: `explore --mapwin` (dummy video driver) must draw the ROM map at least once
# (checked through the DEEPEEL_MAPWIN_DUMP PPM hook). Builds without SDL2 report it and the test passes.
out="${TMPDIR:-/tmp}/mapwin_$$.ppm"
rm -f "$out"
msg=$(SDL_VIDEODRIVER=dummy DEEPEEL_MAPWIN_DUMP="$out" "$1" explore "$2" --mapwin 1 --text /dev/null 2>&1 >/dev/null)
case "$msg" in *"without SDL2"*) rm -f "$out"; exit 0 ;; esac
head -c 2 "$out" 2>/dev/null | grep -q P6 || { echo "no map drawn"; rm -f "$out"; exit 1; }
rm -f "$out"
