#!/usr/bin/env bash
# PS5SX2 (fix180, AI-assisted): what a press on a PS2 disc tile starts (orbis-shims/OrbisDiscTileStart.h) on a PC: the dump by serial, else the disc, never
# the question; a half-finished copy doesn't count. And main-boot.cpp / ProsperoDiscDump.cpp use it as the test assumes.
#   ps5/coreorbis/tests/disctile/test-disctile.sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
shims="$here/../../orbis-shims"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
CXX=${CXX:-clang++-18}
"$CXX" -std=c++20 -O1 -g -Wall -Wextra -Werror -I"$shims" "$here/test_disctile.cpp" -o "$work/test_disctile"
"$work/test_disctile"
missing=0
pin() {
	if ! grep -qF -- "$2" "$1"; then
		echo "FAIL  $(basename "$1") no longer has: $2"
		missing=1
	fi
}
pin "$here/../../main-boot.cpp" 'const std::string scanned = orbis_disc::PickTileCopy(cands, serial, exists);'
pin "$here/../../main-boot.cpp" 'switch (orbis_disc::DecideTileStart(serial, copy))'
pin "$shims/ProsperoDiscDump.cpp" 'if (s_tile_process.load()) // fix180'
[ "$missing" = 0 ] || exit 1
echo "disctile glue: main-boot.cpp and ProsperoDiscDump.cpp use the decision"
