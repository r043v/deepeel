#!/bin/sh
# mapwin_kernel.sh DEEPEEL FIXDIR: the kernel's live ROM map (--mapwin) draws on every platform through the same
# generic window (dummy video driver, checked through the DEEPEEL_MAPWIN_DUMP PPM hook). Builds without SDL2 pass.
d="$1"; b="$2"; out="${TMPDIR:-/tmp}/mapwin_k_$$.ppm"
for spec in "nes explore $b/fix_nes_branch.nes" "pce explore $b/fix_pce_branch.pce" "md explore $b/fix_md_alias.md" "z80 explore sms $b/fix_z80_branch.sms" "explore $b/fix_branch.gb"; do
  rm -f "$out"
  msg=$(SDL_VIDEODRIVER=dummy DEEPEEL_MAPWIN_DUMP="$out" $d $spec --mapwin 1 --attract-frames 0 2>&1 >/dev/null)
  case "$msg" in *"without SDL2"*) rm -f "$out"; exit 0 ;; esac
  head -c 2 "$out" 2>/dev/null | grep -q P6 || { echo "no map drawn: $spec"; echo "$msg" | tail -2; rm -f "$out"; exit 1; }
done
rm -f "$out"
