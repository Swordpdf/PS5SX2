#!/usr/bin/env bash
# PS5 port frontend: builds fe_host (the shelf on a PC, fe_host.cpp) next to this script.
#
#   ps5/frontend/host/build-host.sh
#   VK_ICD_FILENAMES=/opt/pw-browsers/chromium-1194/chrome-linux/vk_swiftshader_icd.json \
#     ps5/frontend/host/fe_host --data /tmp/fe-data --out /tmp/fe-shots "wait 1" "shot shelf"
#
# Needs clang++ (18 or later), zlib's headers and the machine's libvulkan.so.1; SwiftShader (from a Chromium build)
# stands in for a GPU.
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fe=$(cd -- "$here/.." && pwd)
pcsx2=$(cd -- "$fe/../.." && pwd)
obj=${FE_HOST_OBJ:-$here/obj}
CXX=${CXX:-clang++-18}
CC=${CC:-clang-18}
mkdir -p "$obj"

flags=(-O1 -g -Wall -Wno-unused-function -I"$fe" -I"$pcsx2/pcsx2" -I"$pcsx2/3rdparty/vulkan/include" -I"$pcsx2/3rdparty/libchdr/include"
  -I"$pcsx2/ps5/third_party/lz4" -I"$pcsx2/3rdparty/rapidjson/include" -I"$pcsx2/ps5/third_party/libarchive/libarchive")
objs=()
for src in fe_app fe_covers fe_games fe_renderer fe_text fe_vk fe_i18n fe_settings fe_options fe_sound fe_texpacks fe_patchdl fe_bios; do
  o="$obj/$src.o"
  if [[ ! -f $o || $fe/$src.cpp -nt $o || -n $(find "$fe" -maxdepth 1 -name '*.h' -newer "$o" -print -quit) ]]; then
    "$CXX" -std=c++20 "${flags[@]}" -c "$fe/$src.cpp" -o "$o"
  fi
  objs+=("$o")
done
"$CC" -O1 -I"$fe/third_party/qrcodegen" -c "$fe/third_party/qrcodegen/qrcodegen.c" -o "$obj/qrcodegen.o"
"$CC" -O1 -c "$pcsx2/ps5/third_party/lz4/lz4.c" -o "$obj/lz4.o"
"$CC" -O1 -I"$pcsx2/3rdparty/libchdr/include" -c "$here/chd_stub.c" -o "$obj/chd_stub.o"
# 2026-10-05: the vendored libarchive (its Linux config), for the texture packs (fe_texpacks.cpp).
la="$pcsx2/ps5/third_party/libarchive"
la_objs=()
for c in "$la"/libarchive/*.c; do
  o="$obj/la_$(basename "$c" .c).o"
  [[ -f $o && ! $c -nt $o ]] || "$CC" -O1 -w -DHAVE_CONFIG_H -I"$la" -I"$la/libarchive" -c "$c" -o "$o"
  la_objs+=("$o")
done
"$CXX" -std=c++20 "${flags[@]}" -c "$here/fe_host.cpp" -o "$obj/fe_host.o"
"$CXX" -o "$here/fe_host" "$obj/fe_host.o" "${objs[@]}" "$obj/qrcodegen.o" "$obj/lz4.o" "$obj/chd_stub.o" "${la_objs[@]}" -lz -ldl -lpthread
echo "built $here/fe_host"
