#!/usr/bin/env bash
# PS5SX2 2.41 (AI-assisted): fe_pnach.h, which .pnach files of patches/ and cheats/ belong to a game (the rules, the version check,
# the folder walk, the CRC cache), with ASAN + UBSAN.
#   ps5/frontend/host/test-pnach.sh
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fe=$(cd -- "$here/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/pnach-test.XXXXXX")
trap 'rm -rf "$work"' EXIT
cxx=${CXX:-$(command -v clang++-18 || command -v clang++)}
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$fe" "$here/pnach_test.cpp" -o "$work/pnach_test"
"$work/pnach_test" "$work/files"
