#!/usr/bin/env bash
# PS5 port frontend: the online patches and cheats (fe_patchdl.cpp) and the sheet's Cheats rows, on a PC, against a stand-in
# for raw.githubusercontent.com (patchdl_test.cpp). 2026-10-08 (AI-assisted).
#
#   ps5/frontend/host/test-patchdl.sh
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fe=$(cd -- "$here/.." && pwd)
pcsx2=$(cd -- "$fe/../.." && pwd)
work=${FE_TEST_DIR:-$here/obj/patchdl-test}
CXX=${CXX:-clang++-18}
CC=${CC:-clang-18}
rm -rf "$work"
mkdir -p "$work"

flags=(-O1 -g -Wall -Wno-unused-function -I"$fe" -I"$pcsx2/3rdparty/vulkan/include" -I"$pcsx2/3rdparty/libchdr/include"
  -I"$pcsx2/ps5/third_party/lz4")
objs=()
for src in fe_games fe_covers fe_text fe_i18n fe_settings fe_options fe_patchdl; do
  "$CXX" -std=c++20 "${flags[@]}" -c "$fe/$src.cpp" -o "$work/$src.o"
  objs+=("$work/$src.o")
done
"$CC" -O1 -c "$pcsx2/ps5/third_party/lz4/lz4.c" -o "$work/lz4.o"
"$CC" -O1 -I"$pcsx2/3rdparty/libchdr/include" -c "$here/chd_stub.c" -o "$work/chd_stub.o"
"$CXX" -std=c++20 "${flags[@]}" "$here/patchdl_test.cpp" "${objs[@]}" "$work/lz4.o" "$work/chd_stub.o" -o "$work/patchdl_test" -lz -lpthread
"$work/patchdl_test" "$work/root"
