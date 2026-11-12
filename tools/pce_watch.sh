#!/bin/sh
# pce_watch.sh ROM.pce MOVIE.bk2... : each movie in turn in the watch window, two tiles: m0boy (deepeel) and
# BizHawk (PCEHawk or TurboNyma by the movie's Core, tools/bizhawk_dsy through the shared reference feed).  Each core runs at its own pace
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
  # the reference core is the one that recorded the movie: PCEHawk (BizHawk <= 2.4) or TurboNyma (Mednafen)
  core=${PCE_REF_CORE:-$(unzip -p "$film" Header.txt 2>/dev/null | grep -q "Core PCEHawk" && echo pcehawk || echo pce)}
  echo "watch: reference core $core" >&2
  # a third tile (the other BizHawk core on the same inputs) only on request: PCE_WATCH_THIRD=1
  feed2=$TMPDIR/pce_watch.feed2; bhpid2=; ref2=; other=
  if [ "${PCE_WATCH_THIRD:-0}" = 1 ]; then
    other=$([ "$core" = pcehawk ] && echo pce || echo pcehawk)
    cp "$feed" "$feed2"
    LD_LIBRARY_PATH=${BIZHAWK_DIR:-$HOME/dev/BizHawk}/Assets/dll BIZHAWK_FEED="$feed2" BIZHAWK_FEED_SYNC=1 \
      mono "$bh" "$other" "$rom" "$masks" "$TMPDIR/pce_watch_bh2.dsy" "$n" >/dev/null 2>&1 &
    bhpid2=$!; ref2=$feed2
  fi
  LD_LIBRARY_PATH=${BIZHAWK_DIR:-$HOME/dev/BizHawk}/Assets/dll BIZHAWK_FEED="$feed" BIZHAWK_FEED_SYNC=1 \
    mono "$bh" "$core" "$rom" "$masks" "$TMPDIR/pce_watch_bh.dsy" "$n" >/dev/null 2>&1 &
  bhpid=$!
  env DEEPEEL_PCE_REF_CORE="$core" ${ref2:+DEEPEEL_MD_REF2_FEED="$ref2"} DEEPEEL_MD_REF2_NAME="BizHawk $([ "$other" = pcehawk ] && echo PCEHawk || echo TurboNyma)" \
  DEEPEEL_MD_REF_FEED="$feed" DEEPEEL_MD_REF_NAME="BizHawk $([ "$core" = pcehawk ] && echo PCEHawk || echo TurboNyma)" \
    "$root/build/deepeel" pce explore "$rom" --tas "$film" --play "$n" --watch 30 || true
  kill $bhpid ${bhpid2:-} 2>/dev/null || true
done
