#!/bin/sh
# md_watch.sh ROM.md MOVIE.gmv [frames]: the generic deepeel watch window with BizHawk (Genesis Plus GX core) as the
# reference tile, replaying the same raw GMV; the tiles advance in lockstep (two frames of slack, the slowest core dictates the pace); the window shows 60 fps at most. Runs until closed.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
rom=$1; film=$2
D=${DEEPEEL_DATA:-/run/media/m/Windows/deepeel-data}
export TMPDIR=${TMPDIR:-$D/tmp}; mkdir -p "$TMPDIR"
bh=$root/tools/bizhawk_dsy/bin/Release/net48/bizhawk_dsy.exe
[ -f "$(dirname "$bh")/dll/gpgx.wbx" ] || { mkdir -p "$(dirname "$bh")/dll"; zstd -d -q "${BIZHAWK_DIR:-$HOME/dev/BizHawk}/Assets/dll/gpgx.wbx.zst" -o "$(dirname "$bh")/dll/gpgx.wbx"; }
masks=$TMPDIR/md_watch.masks; feed=$TMPDIR/md_watch.feed
case "$film" in
  *.bk2) python3 -I "$root/tools/tas_masks.py" "$film" "$masks"   # BizHawk film: C rides the slot the Genesis core names "P1 C" (Select's)
         python3 -I -c "
import struct,sys
f=open(sys.argv[1],'wb'); f.write(struct.pack('<I',0x52464D44)); f.write(b'\\0'*(36-4+426*262*4))" "$feed" ;;
  *) python3 -I - "$film" "$masks" "$feed" <<'P'
import struct, sys
d = open(sys.argv[1], 'rb').read()[64:]
out = bytearray()
for i in range(0, len(d) - 2, 3):
    g = ~d[i] & 0xff   # GMV: U D L R B C A Start -> BizHawk slots: U D L R B A C Start
    out.append((g & 0x9f) | ((g >> 1) & 0x20 if g & 0x40 else 0) | (0x40 if g & 0x20 else 0))
open(sys.argv[2], 'wb').write(out)
f = open(sys.argv[3], 'wb'); f.write(struct.pack('<I', 0x52464D44)); f.write(b'\0' * (36 - 4 + 426 * 262 * 4))
P
  ;;
esac
n=$(stat -c %s "$masks"); [ $# -ge 3 ] && n=$3
BIZHAWK_DSY_RANGE=${BIZHAWK_DSY_RANGE:-0-0} LD_LIBRARY_PATH=${BIZHAWK_DIR:-$HOME/dev/BizHawk}/Assets/dll BIZHAWK_FEED="$feed" BIZHAWK_FEED_SYNC=1 \
  mono "$bh" gen "$rom" "$masks" "$TMPDIR/md_watch_bh.dsy" "$n" >"$TMPDIR/md_watch_bh.log" 2>&1 &
bhpid=$!
env DP_MD_REF_LIVE=none DEEPEEL_MD_REF_FEED="$feed" DEEPEEL_MD_REF_NAME="BizHawk Genesis Plus GX" \
  "$root/build/deepeel" md explore "$rom" --tas "$film" --watch 1 --max-total 2000000000 --text "$TMPDIR/md_watch.txt" || true
kill $bhpid 2>/dev/null || true
