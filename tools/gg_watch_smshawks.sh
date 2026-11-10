#!/bin/sh
# Compare m0boy with the SMSHawk cores from BizHawk 1.11.4 and the local tree.
set -eu
if [ "$#" -ne 3 ]; then echo "usage: $0 ROM movie.bk2 frames" >&2; exit 2; fi
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
hawk=${BIZHAWK_DIR:-"$root/../BizHawk"}
runtime=${M0BOY_DIR:-"$root/../m0boy"}
legacy="$root/build/smshawk-1.11.4"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/deepeel-hawks.XXXXXXXX")
oldpid= newpid=
cleanup() {
    status=$?
    [ -z "$oldpid" ] || kill "$oldpid" 2>/dev/null || :
    [ -z "$newpid" ] || kill "$newpid" 2>/dev/null || :
    if [ "$status" -ne 0 ]; then
        [ ! -s "$tmp/old.log" ] || tail -20 "$tmp/old.log" >&2
        [ ! -s "$tmp/current.log" ] || tail -20 "$tmp/current.log" >&2
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM
if [ ! -f "$legacy/legacy.exe" ] || [ "$root/tools/smshawk_1114_dump.cs" -nt "$legacy/legacy.exe" ]; then
    mkdir -p "$legacy"
    if [ ! -f "$legacy/oldsmshawk.dll" ]; then
        git -C "$hawk" archive 1.11.4 | tar -x -C "$legacy"
        sed 's/\$WCREV\$/0/g; s/\$WCBRANCH\$/local/g; s/\$WCSHORTHASH\$/6b20e9e/g' "$legacy/Version/svnrev_template" > "$legacy/Version/svnrev.cs"
        xbuild "$legacy/BizHawk.Emulation.Common/BizHawk.Emulation.Common.csproj" /p:Configuration=Release /verbosity:quiet
        printf '%s\n' 'namespace BizHawk.Emulation.Cores { public class CoreConstructorAttribute : System.Attribute { public CoreConstructorAttribute(params string[] systems) {} } }' > "$legacy/CoreConstructorStub.cs"
        mcs -target:library -optimize+ -unsafe -out:"$legacy/oldsmshawk.dll" \
            -r:"$legacy/BizHawk.Emulation.Common/bin/Release/BizHawk.Common.dll" \
            -r:"$legacy/BizHawk.Emulation.Common/bin/Release/BizHawk.Emulation.Common.dll" \
            "$legacy"/BizHawk.Emulation.Cores/Consoles/Sega/SMS/*.cs \
            "$legacy"/BizHawk.Emulation.Cores/CPUs/Z80/*.cs "$legacy/CoreConstructorStub.cs"
    fi
    mcs -optimize+ -out:"$legacy/legacy.exe" -r:"$legacy/oldsmshawk.dll" \
        -r:"$legacy/BizHawk.Emulation.Common/bin/Release/BizHawk.Common.dll" \
        -r:"$legacy/BizHawk.Emulation.Common/bin/Release/BizHawk.Emulation.Common.dll" "$root/tools/smshawk_1114_dump.cs"
fi
dotnet build "$root/tools/smshawk_dump/smshawk_dump.csproj" -c Release --nologo -v quiet -p:NuGetAudit=false -p:BizHawkDir="$hawk"
cc -O2 -std=c99 -D_POSIX_C_SOURCE=200809L -I"$root/src/platform/sega" -I"$root/src/core" \
    "$root/tools/mkmask.c" "$root/build/libdeepeel_sega.a" "$root/build/libdeepeel_gg.a" \
    "$root/build/libdeepeel_z80.a" "$root/build/libdeepeel_sms.a" -lm -ldl -o "$tmp/mkmask"
"$tmp/mkmask" "$2" "$tmp/masks.bin" "$3"
mkfifo "$tmp/old" "$tmp/current"
ln -s /dev/null "$tmp/current.hawk.bin"
MONO_PATH="$legacy/BizHawk.Emulation.Common/bin/Release" LD_LIBRARY_PATH="$hawk/Assets/dll${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    mono "$legacy/legacy.exe" "$1" "$tmp/masks.bin" "$tmp/old" >"$tmp/old.log" 2>&1 & oldpid=$!
LD_LIBRARY_PATH="$hawk/Assets/dll${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" SMSSHAWK_RAW="$tmp/current" \
    mono "$root/tools/smshawk_dump/bin/Release/net48/smshawk_dump.exe" "$1" "$tmp/masks.bin" "$3" "$tmp/current" >"$tmp/current.log" 2>&1 & newpid=$!
DP_SEGA_SYS=sms DP_GG_WINDOW=1 DP_GG_HAWK_COMPAT=1 \
DP_GG_OLD_HAWK_FRAMES="$tmp/old" DP_GG_HAWK_FRAMES="$tmp/current" \
DP_GG_HAWK_VERSION="$(git -C "$hawk" describe --tags --always --dirty)" \
DP_GG_M0BOY_VERSION="$(git -C "$runtime" describe --tags --always --dirty)" \
    sh "$root/tools/gg_compare_reference.sh" "$1" "$2" "$3"
