#!/usr/bin/env bash
# PS5 port frontend: the serial cache: a second start opens no image (serialcache_test.cpp). 2.02 (AI-assisted).
#
#   ps5/frontend/host/test-serialcache.sh
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fe=$(cd -- "$here/.." && pwd)
pcsx2=$(cd -- "$fe/../.." && pwd)
work=${FE_TEST_DIR:-$here/obj/serialcache-test}
CXX=${CXX:-clang++-18}
CC=${CC:-clang-18}
rm -rf "$work"
mkdir -p "$work"
flags=(-O1 -g -Wall -Wno-unused-function -I"$fe" -I"$pcsx2/3rdparty/vulkan/include" -I"$pcsx2/3rdparty/libchdr/include"
  -I"$pcsx2/ps5/third_party/lz4")
"$CC" -O1 -c "$pcsx2/ps5/third_party/lz4/lz4.c" -o "$work/lz4.o"
"$CC" -O1 -I"$pcsx2/3rdparty/libchdr/include" -c "$here/chd_stub.c" -o "$work/chd_stub.o"
"$CXX" -std=c++20 "${flags[@]}" "$here/serialcache_test.cpp" "$fe/fe_games.cpp" "$fe/fe_covers.cpp" "$fe/fe_text.cpp" "$fe/fe_i18n.cpp" \
  "$fe/fe_settings.cpp" "$work/lz4.o" "$work/chd_stub.o" -o "$work/serialcache_test" -lz -lpthread
"$work/serialcache_test" "$work/run"
rm -rf "$work/run"
