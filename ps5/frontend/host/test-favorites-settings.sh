#!/usr/bin/env bash
# Favorites persistence and native/web option parity. Synthetic fixtures only. (AI-assisted)
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fe=$(cd -- "$here/.." && pwd)
root=$(cd -- "$fe/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
CXX=${CXX:-clang++-18}
CC=${CC:-clang-18}
flags=(-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -I"$fe"
  -I"$root/3rdparty/libchdr/include" -I"$root/ps5/third_party/lz4")
objs=()
for src in fe_games fe_options fe_settings fe_text fe_i18n; do
  "$CXX" "${flags[@]}" -c "$fe/$src.cpp" -o "$work/$src.o"
  objs+=("$work/$src.o")
done
"$CC" -O1 -c "$root/ps5/third_party/lz4/lz4.c" -o "$work/lz4.o"
"$CC" -O1 -I"$root/3rdparty/libchdr/include" -c "$here/chd_stub.c" -o "$work/chd_stub.o"
"$CXX" "${flags[@]}" "$here/favorites_test.cpp" "${objs[@]}" "$work/lz4.o" "$work/chd_stub.o" -lz -lpthread -o "$work/favorites_test"
"$work/favorites_test" "$work/data"
"$CXX" "${flags[@]}" "$here/options_dump.cpp" "${objs[@]}" "$work/lz4.o" "$work/chd_stub.o" -lz -lpthread -o "$work/options_dump"
"$work/options_dump" > "$work/native-options.txt"
node "$here/options_parity.js" "$fe/assets/web/index.html" > "$work/web-options.txt"
diff -u "$work/native-options.txt" "$work/web-options.txt"
node "$here/favorites_page_test.js" "$fe/assets/web/index.html"
echo 'PASS: Favorites settings and native/web option parity'
