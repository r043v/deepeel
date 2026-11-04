#!/bin/sh
# lr_play_cli.sh DEEPEEL CORE: `lr play` must report an identical replay after load;
# `lr novelty` must run and report at least one screen cell.
rom="${TMPDIR:-/tmp}/lr_play_$$.bin"
head -c 256 /dev/zero > "$rom"
"$1" lr play "$2" "$rom" --frames 20 | grep -q "replay after load: identical"
rc=$?
if [ $rc -eq 0 ]; then
  "$1" lr novelty "$2" "$rom" --rounds 20 | grep -q "cells reached: [1-9]"
  rc=$?
fi
if [ $rc -eq 0 ]; then
  # human mode exists only in GUI builds (usage error 2 otherwise); dummy video driver, 10 frames
  SDL_VIDEODRIVER=dummy "$1" lr watch "$2" "$rom" --max-frames 10 >/dev/null 2>&1
  w=$?
  [ $w -eq 0 ] || [ $w -eq 2 ] || rc=1
fi
if [ $rc -eq 0 ]; then
  # live view of a novelty run (dummy video driver): must still report its cells
  SDL_VIDEODRIVER=dummy "$1" lr novelty "$2" "$rom" --rounds 20 --watch 1 | grep -q "cells reached: [1-9]" || rc=1
fi
rm -f "$rom"
exit $rc
