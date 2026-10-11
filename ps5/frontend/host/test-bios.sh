#!/usr/bin/env bash
# PS5 port frontend: the BIOS before the shelf (fe_bios.cpp), on a PC (bios_test.cpp). vk-285-134 (AI-assisted).
#
#   ps5/frontend/host/test-bios.sh
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fe=$(cd -- "$here/.." && pwd)
pcsx2=$(cd -- "$fe/../.." && pwd)
work=${FE_TEST_DIR:-$here/obj/bios-test}
CXX=${CXX:-clang++-18}
CC=${CC:-clang-18}
rm -rf "$work"
mkdir -p "$work/la"

la="$pcsx2/ps5/third_party/libarchive"
la_objs=()
for c in "$la"/libarchive/*.c; do
  o="$work/la/$(basename "$c" .c).o"
  "$CC" -O1 -w -DHAVE_CONFIG_H -I"$la" -I"$la/libarchive" -c "$c" -o "$o"
  la_objs+=("$o")
done
flags=(-O1 -g -Wall -Wextra -I"$fe" -I"$la/libarchive" ${FE_SANITIZE:+-fsanitize=$FE_SANITIZE})
"$CXX" -std=c++20 "${flags[@]}" "$here/bios_test.cpp" "$fe/fe_bios.cpp" "${la_objs[@]}" -o "$work/bios_test" -lz -lpthread
"$work/bios_test" "$work/root"
