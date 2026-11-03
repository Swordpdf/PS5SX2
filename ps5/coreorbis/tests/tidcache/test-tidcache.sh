#!/usr/bin/env bash
# PS5SX2 (187, AI-assisted): builds and runs the PC test of the EE's GetThreadId cache and its invalidation (pcsx2/OrbisEEHle.h).
#   ps5/coreorbis/tests/tidcache/test-tidcache.sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../../.." && pwd)
obj=$(mktemp -d)
trap 'rm -rf "$obj"' EXIT
CXX=${CXX:-clang++-18}
flags=(-std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -I"$root/pcsx2" -I"$root")
"$CXX" "${flags[@]}" -fsanitize=address,undefined "$here/test_tidcache.cpp" -o "$obj/test_tidcache" ||
  "$CXX" "${flags[@]}" "$here/test_tidcache.cpp" -o "$obj/test_tidcache"
"$obj/test_tidcache" | tee "$obj/out.txt"
grep -q "^ALL PASSED" "$obj/out.txt"
