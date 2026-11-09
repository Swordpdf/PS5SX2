#!/usr/bin/env bash
# Exercise real native settings persistence and the web page's setting/save functions. (AI-assisted)
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fe=$(cd -- "$here/.." && pwd)
root=$(cd -- "$fe/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
CXX=${CXX:-clang++-18}
CC=${CC:-clang-18}
flags=(-O1 -g -Wall -Wno-unused-function -I"$fe" -I"$root/3rdparty/vulkan/include"
  -I"$root/3rdparty/libchdr/include" -I"$root/ps5/third_party/lz4")
objs=()
for src in fe_games fe_covers fe_text fe_i18n fe_settings fe_options; do
  "$CXX" -std=c++20 "${flags[@]}" -c "$fe/$src.cpp" -o "$work/$src.o"
  objs+=("$work/$src.o")
done
"$CC" -O1 -c "$root/ps5/third_party/lz4/lz4.c" -o "$work/lz4.o"
"$CC" -O1 -I"$root/3rdparty/libchdr/include" -c "$here/chd_stub.c" -o "$work/chd_stub.o"
"$CXX" -std=c++20 "${flags[@]}" "$here/dithering_test.cpp" "${objs[@]}" "$work/lz4.o" "$work/chd_stub.o" \
  -o "$work/dithering_test" -lz -lpthread
mkdir "$work/settings-root"
"$work/dithering_test" "$work/settings-root"
node "$here/dithering_page_test.js" "$fe/assets/web/index.html"
