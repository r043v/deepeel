#!/bin/sh
# lr_play_cli.sh DEEPEEL CORE: `lr play` must report an identical replay after load.
rom="${TMPDIR:-/tmp}/lr_play_$$.bin"
head -c 256 /dev/zero > "$rom"
"$1" lr play "$2" "$rom" --frames 20 | grep -q "replay after load: identical"
rc=$?
rm -f "$rom"
exit $rc
