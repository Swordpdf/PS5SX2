#!/usr/bin/env bash
# vk-285-139 (AI-assisted): fe::DiscSet / DiscNumber on the PC (ASan/UBSan; CXX=g++ where clang has no sanitizer runtimes).
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fe=$(cd -- "$here/.." && pwd)
pcsx2=$(cd -- "$fe/../.." && pwd)
CXX=${CXX:-clang++-18}
CC=${CC:-clang-18}
work=$(mktemp -d "${TMPDIR:-/tmp}/discs.XXXXXX")
trap 'rm -rf "$work"' EXIT
san=(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
"$CC" -O1 -g -c "$pcsx2/ps5/third_party/lz4/lz4.c" -o "$work/lz4.o"
"$CC" -O1 -g -I"$pcsx2/3rdparty/libchdr/include" -c "$here/chd_stub.c" -o "$work/chd_stub.o"
"$CXX" -std=c++20 -O1 -g -Wall -Wno-unused-function -I"$fe" -I"$pcsx2/pcsx2" -I"$pcsx2/3rdparty/rapidjson/include" \
  -I"$pcsx2/3rdparty/vulkan/include" -I"$pcsx2/3rdparty/libchdr/include" -I"$pcsx2/ps5/third_party/lz4" "${san[@]}" \
  "$here/discs_test.cpp" "$fe/fe_games.cpp" "$fe/fe_settings.cpp" "$fe/fe_covers.cpp" "$fe/fe_text.cpp" "$fe/fe_i18n.cpp" \
  "$work/lz4.o" "$work/chd_stub.o" -lz -lpthread -o "$work/discs_test"
mkdir -p "$work/run"
"$work/discs_test" "$work/run"
