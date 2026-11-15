#!/bin/sh
# NES coverage techniques on the generic kernel: one fixture per technique, the kernel
# report line that proves it.  usage: nes_kernel_fixtures.sh deepeel build_dir work_dir
D=$1; B=$2; W=$3; rc=0
chk() {   # fixture, extra args, regex
    f=$1; a=$2; re=$3
    # shellcheck disable=SC2086
    "$D" nes explore "$B/fix_nes_$f.nes" --attract-frames 0 --max-total 200000 --max-steps 1000 $a --text "$W/nk_$f.txt" >/dev/null 2>&1
    grep -Eq "$re" "$W/nk_$f.txt" || { echo "FAIL $f [$a]: $re"; rc=1; }
}
chk static   "--max-steps 2"      'static closure: roots 2, bytes 10'
chk table    ""                   'jump tables 1 \(6 bytes\)'
chk mmc1table ""                  'jump tables 1 \(6 bytes\)'
chk sibling  ""                   'copy tables 1 '
chk xwin     "--max-steps 2"      'static closure: roots 2, bytes 12'
chk cpucopy  ""                   'rom->ram copy bytes [1-9]'
chk wramcopy ""                   'rom->ram copy bytes [1-9]'
chk sramcopy ""                   'rom->ram copy bytes [1-9]'
chk apucopy  ""                   'rom->ram copy bytes [1-9]'
chk oamchain ""                   'rom->ram copy bytes [1-9]'
chk branch   "--entry 0:8150 --entry 3:8000" 'seeded entry points 2'
chk uxrom    "--seed-banks"       'bank seeds the machine cannot show'
chk pad      ""                   'event probes 72 at 3 sites'
chk pad      "--pad-full"         'event probes 2304 at 3 sites'
exit $rc
