#!/usr/bin/env bash
# PS5 port: PCSX2's game database inside the eboot, behind resources/GameIndex.yaml (include-orbis/OrbisGameDb.h). gamedb, 2026-10-30
# (AI-assisted). orbis-shims/orbis_gamedb.cpp is built as the eboot builds it (.incbin of bin/resources/GameIndex.yaml).
#   ps5/coreorbis/tests/gamedb/test-gamedb.sh
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
CXX=${CXX:-clang++-18}
db="$root/bin/resources/GameIndex.yaml"

flags=(-std=c++20 -O1 -g -Wall -Wextra -Werror -I"$root/ps5/coreorbis/include-orbis")
"$CXX" "${flags[@]}" -DORBIS_GAMEDB_FILE="\"$db\"" -c "$root/ps5/coreorbis/orbis-shims/orbis_gamedb.cpp" -o "$work/orbis_gamedb.o"
"$CXX" "${flags[@]}" "$here/gamedb_test.cpp" "$work/orbis_gamedb.o" -o "$work/gamedb_test"
"$work/gamedb_test" "$work/root" "$db"
