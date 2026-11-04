#!/bin/sh
# lr_gpgx_trace.sh DEEPEEL FIXTURE_FORK_MD: level-B parity on a real core (Genesis Plus GX patched with
# tools/gpgx_trace.patch, built with HOOK_CPU=1). The core is outside the repository: set DP_GPGX_TRACE_CORE
# to its .so; without it the test is skipped (exit 0).
[ -n "$DP_GPGX_TRACE_CORE" ] && [ -f "$DP_GPGX_TRACE_CORE" ] || { echo "skipped: DP_GPGX_TRACE_CORE not set"; exit 0; }
out=$("$1" lr explore "$DP_GPGX_TRACE_CORE" "$2" 2>&1) || true
echo "$out" | grep -q "level B" || { echo "no level B"; exit 1; }
# fix_md_fork: tst.w d0 / bne.s / nop / bra.s * / nop / bra.s * : both sides = 6 opcode starts, 1 fork site
echo "$out" | grep -q "code starts reached: 6 of" || { echo "$out"; exit 1; }
echo "$out" | grep -q "branch sites 1" || { echo "$out"; exit 1; }
