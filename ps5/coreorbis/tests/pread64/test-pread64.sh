#!/usr/bin/env bash
# PS5SX2 (vk-285-179, AI-assisted): builds and runs the PC test of the DiscSpeed8 64 KiB pread chunking (OrbisDiscPread64.h).
#   ps5/coreorbis/tests/pread64/test-pread64.sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../../.." && pwd)
obj=$(mktemp -d)
trap 'rm -rf "$obj"' EXIT
CXX=${CXX:-g++}
"$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$root/ps5/coreorbis/orbis-shims" "$here/test_pread64.cpp" -o "$obj/test_pread64" ||
  "$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror -I"$root/ps5/coreorbis/orbis-shims" "$here/test_pread64.cpp" -o "$obj/test_pread64"
"$obj/test_pread64" | tee "$obj/out.txt"
grep -q "^ALL PASSED" "$obj/out.txt"
