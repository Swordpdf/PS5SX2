#!/usr/bin/env bash
# PS5SX2 (vk-285-179, AI-assisted): builds and runs the PC test of the helper auto-load decision (OrbisHelperAutoload.h).
#   ps5/coreorbis/tests/helperload/test-helperload.sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../../.." && pwd)
obj=$(mktemp -d)
trap 'rm -rf "$obj"' EXIT
CXX=${CXX:-g++}
"$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$root/ps5/coreorbis/orbis-shims" "$here/test_helperload.cpp" -o "$obj/test_helperload" ||
  "$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror -I"$root/ps5/coreorbis/orbis-shims" "$here/test_helperload.cpp" -o "$obj/test_helperload"
"$obj/test_helperload" | tee "$obj/out.txt"
grep -q "^ALL PASSED" "$obj/out.txt"
