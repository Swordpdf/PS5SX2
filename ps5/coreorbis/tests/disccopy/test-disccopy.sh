#!/usr/bin/env bash
# PS5 port: the disc copy's read-ahead loop (orbis-shims/OrbisDiscCopy.h) on a PC. vk-285-147 (AI-assisted).
#   ps5/coreorbis/tests/disccopy/test-disccopy.sh   (DISCCOPY_SANITIZE=thread or address,undefined)
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
work=${DISCCOPY_TEST_DIR:-$here/obj}
CXX=${CXX:-clang++-18}
mkdir -p "$work"
san=()
[[ -n ${DISCCOPY_SANITIZE:-} ]] && san=(-fsanitize="$DISCCOPY_SANITIZE" -fno-omit-frame-pointer)
"$CXX" -std=c++20 -O1 -g -Wall -Wextra "${san[@]}" "$here/test_disccopy.cpp" -o "$work/test_disccopy" -lpthread
"$work/test_disccopy"
