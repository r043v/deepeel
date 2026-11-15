#!/bin/sh
# watch_bizhawk.sh SYSTEM ROM MOVIE... : the generic deepeel watch window with BizHawk as a live reference tile, for
# every platform (md, pce, nes, sms).  Each MOVIE (.bk2, .fm2, or a raw Gens .gmv on md) plays in turn on both cores
# with the same inputs; the tiles advance in lockstep (two frames of slack, the slowest core dictates the pace), the
# window shows 60 fps at most.  Escape closes the window and goes on to the next movie.
# Environment: DEEPEEL_DATA (scratch/data disk), BIZHAWK_DIR, PCE_REF_CORE (pce|pcehawk), WATCH_THIRD=1 (md: ClownMDEmu live tile too; pce: the other
# BizHawk core as a third tile), BIZHAWK_INPUT_DELAY (k idle frames before the movie on the BizHawk side, for a slower boot), WATCH_FRAMES (stop after N frames), BIZHAWK_DSY_RANGE (default 0-0: no memory dump).
set -eu
[ $# -ge 3 ] || { echo "usage: $0 md|pce|nes|sms ROM MOVIE..." >&2; exit 2; }
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
sys=$1; rom=$2; shift 2
D=${DEEPEEL_DATA:-/run/media/m/Windows/deepeel-data}
export TMPDIR=${TMPDIR:-$D/tmp}; mkdir -p "$TMPDIR"
hawk=${BIZHAWK_DIR:-$HOME/dev/BizHawk}
bh=$root/tools/bizhawk_dsy/bin/Release/net48/bizhawk_dsy.exe
# per system: BizHawk core, waterbox image to unpack, deepeel extra options
case $sys in
  md)  core=gen;  wbx=gpgx;  name="BizHawk Genesis Plus GX"; extra="--max-total 2000000000" ;;
  pce) core=pce;  wbx=turbo; name=; extra= ;;
  nes) core=nes;  wbx=;      name="BizHawk NesHawk"; extra= ;;
  sms) core=sms;  wbx=;      name="BizHawk SMSHawk"; extra= ;;
  *) echo "watch_bizhawk: unknown system $sys" >&2; exit 2 ;;
esac
[ -z "$wbx" ] || [ -f "$(dirname "$bh")/dll/$wbx.wbx" ] || { mkdir -p "$(dirname "$bh")/dll"; zstd -d -q "$hawk/Assets/dll/$wbx.wbx.zst" -o "$(dirname "$bh")/dll/$wbx.wbx"; }
feedinit() { python3 -I -c "
import struct,sys
f=open(sys.argv[1],'wb'); f.write(struct.pack('<I',0x52464D44)); f.write(b'\\0'*(36-4+426*262*4))" "$1"; }
for film in "$@"; do
  masks=$TMPDIR/watch_$sys.masks; feed=$TMPDIR/watch_$sys.feed; feed2=$TMPDIR/watch_$sys.feed2
  case $film in
    *.gmv) python3 -I - "$film" "$masks" <<'P'
import sys
d = open(sys.argv[1], 'rb').read()[64:]
out = bytearray()
for i in range(0, len(d) - 2, 3):
    g = ~d[i] & 0xff   # GMV: U D L R B C A Start -> mask bits U D L R B A C(Select slot) Start
    out.append((g & 0x9f) | ((g >> 1) & 0x20 if g & 0x40 else 0) | (0x40 if g & 0x20 else 0))
open(sys.argv[2], 'wb').write(out)
P
    ;;
    *) python3 -I "$root/tools/tas_masks.py" "$film" "$masks" ;;
  esac
  n=$(stat -c %s "$masks"); [ -z "${WATCH_FRAMES:-}" ] || n=$WATCH_FRAMES
  c=$core; refname=$name; ref2=; other=
  if [ "$sys" = pce ]; then   # the core that recorded the movie: PCEHawk (BizHawk <= 2.4) or TurboNyma (Mednafen)
    c=${PCE_REF_CORE:-$(unzip -p "$film" Header.txt 2>/dev/null | grep -q "Core PCEHawk" && echo pcehawk || echo pce)}
    v=$(sed -n 's/.*MainVersion = "\([^"]*\)".*/\1/p' "$hawk/src/BizHawk.Common/VersionInfo.cs" 2>/dev/null | head -1)
    nm() { [ "$1" = pcehawk ] && echo "BizHawk ${v:-?} PCEHawk" || echo "BizHawk ${v:-?} TurboNyma (Mednafen 1.32.1)"; }
    refname=$(nm "$c"); echo "watch: reference core $c" >&2
  fi
  run_bh() {  # core feed log
    BIZHAWK_DSY_RANGE=${BIZHAWK_DSY_RANGE:-0-0} LD_LIBRARY_PATH="$hawk/Assets/dll" BIZHAWK_FEED="$2" BIZHAWK_FEED_SYNC=1 \
      mono "$bh" "$1" "$rom" "$masks" "$TMPDIR/watch_$sys.$1.dsy" "$n" >"$3" 2>&1 &
  }
  feedinit "$feed"; run_bh "$c" "$feed" "$TMPDIR/watch_$sys.log"; bhpid=$!; bhpid2=
  if [ "$sys" = pce ] && [ "${WATCH_THIRD:-0}" = 1 ]; then
    other=$([ "$c" = pcehawk ] && echo pce || echo pcehawk); feedinit "$feed2"
    run_bh "$other" "$feed2" "$TMPDIR/watch_$sys.log2"; bhpid2=$!; ref2=$feed2
  fi
  # (the movie path may hold spaces: no unquoted option string; $extra and the play count hold none)
  if [ "$sys" = md ]; then play=; else play="--play $n"; fi
  if [ "$sys" = md ] && [ "${WATCH_THIRD:-0}" = 1 ]; then   # md third tile: deepeel's own live ClownMDEmu tile (tools/md_ref_live.sh) plus BizHawk
    env DEEPEEL_WATCH_FRAMES="${WATCH_FRAMES:-}" DEEPEEL_REF2_FEED="$feed" DEEPEEL_REF2_NAME="$refname" "$root/build/deepeel" "$sys" explore "$rom" --tas "$film" --watch 60 $extra || true
    kill $bhpid 2>/dev/null || true; continue
  fi
  env DEEPEEL_WATCH_FRAMES="${WATCH_FRAMES:-}" DEEPEEL_REF_FEED="$feed" DEEPEEL_REF_NAME="$refname" ${ref2:+DEEPEEL_REF2_FEED="$ref2" DEEPEEL_REF2_NAME="$(nm "$other")"} \
    DEEPEEL_PCE_REF_CORE="$c" "$root/build/deepeel" "$sys" explore "$rom" --tas "$film" $play --watch 60 $extra || true
  kill $bhpid ${bhpid2:-} 2>/dev/null || true
done
