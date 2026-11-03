#!/usr/bin/env bash
# PS5SX2 (187, AI-assisted): builds the real emitter (common/emitter) and runs the PC tests of build 187's EE recompiler
# changes: the block check (iR5900.cpp, OrbisSmcCheck), the constant-address loads and stores (recVTLB.cpp) and the
# emitter's truncation path (x86emitter.cpp). The code under test is taken from the sources, between their
# 187-SMC-BEGIN/END and 187-VTLB-BEGIN/END marks.
#   ps5/coreorbis/tests/eerec/test-eerec.sh          the tests
#   ps5/coreorbis/tests/eerec/test-eerec.sh bench    and the old/new block check timing (PC numbers, not the PS5's)
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../../.." && pwd)
obj=$(mktemp -d)
trap 'rm -rf "$obj"' EXIT
CXX=${CXX:-clang++-18}
flags=(-std=c++20 -O2 -g -mavx2 -Wall -Wno-unused-function -I"$root" -I"$root/3rdparty/include" -I"$root/3rdparty/fmt/include"
  -I"$obj")

extract() { # source, begin mark, end mark, output
  sed -n "/^\/\/ $2/,/^\/\/ $3/p" "$1" >"$4"
  [ -s "$4" ] || { echo "FAILED: no $2 in $1"; exit 1; }
}
extract "$root/pcsx2/x86/ix86-32/iR5900.cpp" 187-SMC-BEGIN 187-SMC-END "$obj/smc_check.inc"
extract "$root/pcsx2/x86/ix86-32/recVTLB.cpp" 187-VTLB-BEGIN 187-VTLB-END "$obj/vtlb_const.inc"

emitter=()
for f in "$root"/common/emitter/*.cpp; do
  o="$obj/emitter_$(basename "$f" .cpp).o"
  "$CXX" "${flags[@]}" -c "$f" -o "$o"
  emitter+=("$o")
done
"$CXX" "${flags[@]}" -c "$here/stubs.cpp" -o "$obj/stubs.o"
tests=(test_smc_check test_const_access test_sibwarn)
for t in "${tests[@]}"; do
  "$CXX" "${flags[@]}" "$here/$t.cpp" "${emitter[@]}" "$obj/stubs.o" -o "$obj/$t"
done

status=0
for t in "${tests[@]}"; do
  echo "== $t"
  "$obj/$t" | tee "$obj/$t.txt" || status=1
  grep -q "^ALL PASSED" "$obj/$t.txt" || status=1
done
if [ "${1:-}" = bench ]; then
  echo "== bench (PC numbers, not the PS5's)"
  "$obj/test_smc_check" bench
fi
if [ $status = 0 ]; then echo "EEREC TESTS PASSED"; else echo "EEREC TESTS FAILED"; fi
exit $status
