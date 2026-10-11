#!/usr/bin/env bash
# PS5 port: the MMC commands for the PS5's disc drive (orbis-shims/OrbisDiscScsi.h) on a PC. vk-285-150 (AI-assisted).
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
work=${DISCSCSI_TEST_DIR:-$here/obj}
CXX=${CXX:-clang++-18}
mkdir -p "$work"
"$CXX" -std=c++20 -O1 -g -Wall -Wextra "$here/test_discscsi.cpp" -o "$work/test_discscsi"
"$work/test_discscsi"
