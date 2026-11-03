#!/usr/bin/env bash
# PS5SX2 (187, AI-assisted): builds and runs the PC test of the image reader's read failures (test_isoread.cpp; audit io-2):
#   ps5/coreorbis/tests/isoread/test-isoread.sh          the REAL FlatFileReader.cpp + ThreadedFileReader.cpp, ASan/UBSan (about 7 s)
#   ISOREAD_TSAN=1 ps5/coreorbis/tests/isoread/test-isoread.sh    also under ThreadSanitizer
# The failures are injected with ld's --wrap: fread and pread go through test_isoread.cpp first.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../../.." && pwd)
obj=$(mktemp -d)
trap 'rm -rf "$obj"' EXIT
CXX=${CXX:-clang++-18}
inc=(-I"$root" -I"$root/pcsx2" -I"$root/3rdparty/include" -I"$root/3rdparty/fmt/include" -I"$root/common" -I"$root/3rdparty/fast_float/include")
defs=(-D_M_X86=1 -D__POSIX__=1 -D__linux__=1)
srcs=("$here/test_isoread.cpp" "$root/pcsx2/CDVD/FlatFileReader.cpp" "$root/pcsx2/CDVD/ThreadedFileReader.cpp" "$root/3rdparty/fmt/src/format.cc")
build() { # build <out> <sanitizer flags...>
  local out=$1
  shift
  "$CXX" -std=c++20 -O1 -g "${defs[@]}" "${inc[@]}" -include common/Threading.h "$@" "${srcs[@]}" -pthread -Wl,--wrap=fread -Wl,--wrap=pread -o "$out"
}
fail=0
run() { # run <exe> <name>
  mkdir -p "$obj/$2"
  "$1" "$obj/$2" > "$obj/$2.txt" 2>&1 || true
  grep -E "^(PASS|FAIL|ALL PASSED|SOME FAILED)" "$obj/$2.txt"
  grep -q "^ALL PASSED" "$obj/$2.txt" || { fail=1; tail -40 "$obj/$2.txt"; }
}
echo "== 1. the real reader with injected read failures (ASan, UBSan)"
build "$obj/test_isoread" -fsanitize=address,undefined 2>/dev/null || build "$obj/test_isoread"
run "$obj/test_isoread" plain
if [[ ${ISOREAD_TSAN:-0} == 1 ]]; then
  echo "== 2. the same under ThreadSanitizer"
  build "$obj/test_isoread_tsan" -fsanitize=thread
  TSAN_OPTIONS="halt_on_error=0 exitcode=0" run "$obj/test_isoread_tsan" tsan
  if grep -q "WARNING: ThreadSanitizer" "$obj/tsan.txt"; then
    echo "INFO  ThreadSanitizer reports (see which lines are this change's):"
    grep -A14 "WARNING: ThreadSanitizer" "$obj/tsan.txt" | grep -E "WARNING|#[0-9] .*(FlatFileReader|ThreadedFileReader|test_isoread)" | head -60
  else
    echo "PASS  ThreadSanitizer: no report"
  fi
fi
if [[ $fail != 0 ]]; then
  echo "FAILED"
  exit 1
fi
echo "ALL PASSED"
