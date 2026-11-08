#!/bin/sh
# campaign_full.sh: the full per-ROM campaign (axis 4): one bounded run per ROM
# of a platform directory, all outputs (--summary/--cov/--evidence/--romdata
# where the platform has them), results in a versioned directory.  Light by
# construction: --time-limit per ROM, sequential, resumable (existing
# summaries are skipped — rerun to continue).  Numbers are tool observations.
#
#   campaign_full.sh <platform gb|nes|md|sg|sms|gg|msx|pce> <rom-dir> <out-dir> [max-total] [time-limit]
set -eu
plat=$1; romdir=$2; out=$3
maxtotal=${4:-20000000}; timelimit=${5:-600}
B=${DEEPEEL_BIN:-$PWD/build/deepeel}
mkdir -p "$out"
find "$romdir" -type f \( -iname "*.gb" -o -iname "*.gbc" -o -iname "*.nes" -o -iname "*.md" -o -iname "*.sg" -o -iname "*.sms" -o -iname "*.gg" -o -iname "*.rom" -o -iname "*.mx1" -o -iname "*.mx2" -o -iname "*.pce" \) | sort | while read -r rom; do
    case "$rom" in
        *test*|*Test*|*[Bb]ios*|*[Pp]D\ *.sms) ;;  # hardware-test ROMs and BIOSes: dedicated axes, not the campaign
    esac
    slug=$(basename "$rom"); slug=${slug%.*}; slug=$(printf '%s' "$slug" | tr ' ' '_' | tr -cd 'A-Za-z0-9_.-')
    d="$out/$plat/$slug"; mkdir -p "$d"
    [ -f "$d/summary.json" ] && { echo "skip $plat/$slug (done)"; continue; }
    echo "== $plat: $slug"
    case "$plat" in
    gb)   $B explore "$rom" --max-total "$maxtotal" --time-limit "$timelimit" \
              --summary "$d/summary.json" --cov "$d/rom.cov" \
              --rtcode "$d/rtcode.txt" --text "$d/report.txt" >/dev/null 2>&1 || echo "  (rc=$?)";;
    nes)  $B nes explore "$rom" --max-total "$maxtotal" --time-limit "$timelimit" \
              --summary "$d/summary.json" --cov "$d/rom.cov" --evidence "$d/evidence.txt" \
              --romdata "$d/romdata.txt" --text "$d/report.txt" >/dev/null 2>&1 || echo "  (rc=$?)";;
    md)   $B md explore "$rom" --max-total "$maxtotal" --time-limit "$timelimit" \
              --summary "$d/summary.json" --cov "$d/rom.cov" --evidence "$d/evidence.txt" \
              --romdata "$d/romdata.txt" --rtcode "$d/rtcode.txt" --text "$d/report.txt" >/dev/null 2>&1 || echo "  (rc=$?)";;
    sg|sms|gg|msx) $B z80 explore "$plat" "$rom" --max-total "$maxtotal" \
              --summary "$d/summary.json" --cov "$d/rom.cov" --evidence "$d/evidence.txt" \
              --romdata "$d/romdata.txt" --text "$d/report.txt" >/dev/null 2>&1 || echo "  (rc=$?)";;
    pce)  $B pce explore "$rom" --max-total "$maxtotal" --time-limit "$timelimit" \
              --summary "$d/summary.json" --cov "$d/rom.cov" --evidence "$d/evidence.txt" \
              --romdata "$d/romdata.txt" --text "$d/report.txt" >/dev/null 2>&1 || echo "  (rc=$?)";;
    *) echo "unknown platform $plat"; exit 2;;
    esac
    [ -f "$d/summary.json" ] && grep -o '"known_pct":[0-9.]*' "$d/summary.json" | head -1 || echo "  no summary"
done
