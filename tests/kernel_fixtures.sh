#!/bin/sh
# Coverage techniques on the generic kernel: one fixture per technique, the kernel
# report line that proves it.  usage: kernel_fixtures.sh deepeel build_dir work_dir
D=$1; B=$2; W=$3; rc=0
chk() {   # platform fixture, extra args, regex
    p=${1%%:*}; f=${1#*:}; a=$2; re=$3
    # shellcheck disable=SC2086
    case $p in nes) x=nes;; *) x=$p;; esac
    "$D" $p explore "$B/fix_${p}_$f.$x" --attract-frames 0 --max-total 200000 --max-steps 1000 $a --text "$W/k_${p}_$f.txt" >/dev/null 2>&1
    grep -Eq "$re" "$W/k_${p}_$f.txt" || { echo "FAIL $p:$f [$a]: $re"; rc=1; }
}
chk nes:static   "--max-steps 2"      'static closure: roots 2, bytes 10'
chk nes:table    ""                   'jump tables 1 \(6 bytes\)'
chk nes:mmc1table ""                  'jump tables 1 \(6 bytes\)'
chk nes:sibling  ""                   'copy tables 1 '
chk nes:xwin     "--max-steps 2"      'static closure: roots 2, bytes 12'
chk nes:cpucopy  ""                   'rom->ram copy bytes [1-9]'
chk nes:wramcopy ""                   'rom->ram copy bytes [1-9]'
chk nes:sramcopy ""                   'rom->ram copy bytes [1-9]'
chk nes:apucopy  ""                   'rom->ram copy bytes [1-9]'
chk nes:oamchain ""                   'rom->ram copy bytes [1-9]'
chk nes:branch   "--entry 0:8150 --entry 3:8000" 'seeded entry points 2'
chk nes:uxrom    "--seed-banks"       'bank seeds the machine cannot show'
chk nes:pad      ""                   'event probes 72 at 3 sites'
chk nes:pad      "--pad-full"         'event probes 2304 at 3 sites'
chk pce:branch "--entry 0xE018 --entry 0xE025" 'seeded entry points 2'
chk pce:ind    ""                 'static closure: roots 2, bytes 7'
chk pce:prov   ""                 'jump tables 1 \(190 bytes\)'
chk pce:prov   ""                 'rom->ram copy bytes 34'
exit $rc
