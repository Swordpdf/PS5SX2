#!/usr/bin/env bash
# PS5 port host test (2026-10-08, AI-assisted): the SW JIT's function map when its code region runs full (test_function_map.cpp).
#
#   ps5/coreorbis/tests/swjit/test-function-map.sh
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../../.." && pwd)
work=${SWJIT_TEST_DIR:-$here/obj}
CXX=${CXX:-g++}
mkdir -p "$work"
flags=(-std=c++20 -O1 -g -mavx2 -mbmi2 -Wall -Wno-unused-function -Wno-attributes -I"$here/stub" -I"$root/pcsx2" -I"$root"
  -I"$root/3rdparty/fmt/include" -I"$root/3rdparty/include" -I"$root/3rdparty/xbyak" -I"$root/3rdparty/fast_float/include")
"$CXX" "${flags[@]}" "$here/test_function_map.cpp" "$root/pcsx2/GS/Renderers/Common/GSFunctionMap.cpp" -o "$work/test_function_map"
"$work/test_function_map"
