#!/bin/sh
# pce_watch.sh ROM.pce MOVIE.bk2... : each movie in turn in the watch window, two tiles: m0boy (deepeel) and
# BizHawk (TurboNyma, tools/bizhawk_dsy through the shared reference feed).  Each core runs at its own pace
# (BizHawk paced at BIZHAWK_FEED_FPS, default 60); Escape closes the window and goes on to the next movie.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
rom=$1; shift
D=${DEEPEEL_DATA:-/run/media/m/Windows/deepeel-data}
export TMPDIR=${TMPDIR:-$D/tmp}
bh=$root/tools/bizhawk_dsy/bin/Release/net48/bizhawk_dsy.exe
[ -f "$(dirname "$bh")/dll/turbo.wbx" ] || { mkdir -p "$(dirname "$bh")/dll"; zstd -d -q "${BIZHAWK_DIR:-$HOME/dev/BizHawk}/Assets/dll/turbo.wbx.zst" -o "$(dirname "$bh")/dll/turbo.wbx"; }
for film in "$@"; do
  masks=$TMPDIR/pce_watch.masks; feed=$TMPDIR/pce_watch.feed
  python3 -I "$root/tools/tas_masks.py" "$film" "$masks"
  python3 -I -c "
import struct,sys
f=open(sys.argv[1],'wb'); f.write(struct.pack('<I',0x52464D44)); f.write(b'\0'*(36-4+426*262*4))" "$feed"
  n=$(stat -c %s "$masks")
  LD_LIBRARY_PATH=${BIZHAWK_DIR:-$HOME/dev/BizHawk}/Assets/dll BIZHAWK_FEED="$feed" \
    mono "$bh" pce "$rom" "$masks" "$TMPDIR/pce_watch_bh.dsy" "$n" >/dev/null 2>&1 &
  bhpid=$!
  DEEPEEL_MD_REF_FEED="$feed" DEEPEEL_MD_REF_NAME="BizHawk TurboNyma" \
    "$root/build/deepeel" pce explore "$rom" --tas "$film" --play "$n" --watch 30 || true
  kill $bhpid 2>/dev/null || true
done
