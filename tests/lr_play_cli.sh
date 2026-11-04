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
rm -f "$rom"
exit $rc
