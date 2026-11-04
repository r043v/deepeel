#!/bin/sh
# --spool/--resume (aliases of --lot/--lot-drain) on the MD explorer: a walled run leaves a lot,
# --resume drains it. usage: resume_cli.sh <deepeel> <md rom>
set -e
D=$1; W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
"$D" md explore "$2" --spool "$W/md.lot" --max-total 5 >/dev/null 2>&1
test -s "$W/md.lot"
"$D" md explore "$2" --resume "$W/md.lot" 2>&1 | grep -q "1 read back"
