#!/usr/bin/env bash
# PS5 port frontend (2.31, AI-assisted): builds ingame_preview (the in-game menu, fe_ingame.cpp, drawn on a PC the way the GS's UI
# shader draws it) and runs its script: the opening frame by frame, every tab and row kind, the hold ring, the closing. Fails when
# the menu doesn't close or a picture isn't written. Plain C++ on Linux, no GPU.
#
#   ps5/frontend/host/test-ingame-menu.sh [game picture]
#   FE_SHOTS=<dir> keeps the pictures there (else a temporary folder, removed).
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fe=$(cd -- "$here/.." && pwd)
pcsx2=$(cd -- "$fe/../.." && pwd)
CXX=${CXX:-clang++-18}
CC=${CC:-clang-18}
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
out=${FE_SHOTS:-$work/shots}
picture=${1:-}

flags=(-std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-function -I"$fe" -I"$pcsx2/pcsx2"
  -I"$pcsx2/3rdparty/libchdr/include" -I"$pcsx2/ps5/third_party/lz4" -I"$pcsx2/3rdparty/rapidjson/include")
objs=()
for src in fe_ingame fe_text fe_options fe_settings fe_games fe_i18n; do
  "$CXX" "${flags[@]}" -c "$fe/$src.cpp" -o "$work/$src.o"
  objs+=("$work/$src.o")
done
"$CC" -O1 -c "$pcsx2/ps5/third_party/lz4/lz4.c" -o "$work/lz4.o"
"$CC" -O1 -I"$pcsx2/3rdparty/libchdr/include" -c "$here/chd_stub.c" -o "$work/chd_stub.o"
"$CXX" "${flags[@]}" -c "$here/ingame_preview.cpp" -o "$work/ingame_preview.o"
"$CXX" -o "$work/ingame_preview" "$work/ingame_preview.o" "${objs[@]}" "$work/lz4.o" "$work/chd_stub.o" -lz -lpthread

if [[ -z $picture ]]; then
  # No picture given: a made-up one (sky, ground, a road), so the test needs nothing but this tree.
  python3 - "$work/picture.png" <<'EOF'
import struct, sys, zlib
w, h = 640, 360
rows = []
for y in range(h):
    row = bytearray([0])
    for x in range(w):
        if y < h * 0.55:
            t = y / (h * 0.55)
            c = (int(120 + 60 * t), int(170 + 40 * t), int(230 - 20 * t))
        else:
            t = (y - h * 0.55) / (h * 0.45)
            road = abs(x - w / 2) < 40 + t * 260
            c = (90, 90, 96) if road else (int(70 - 30 * t), int(110 - 40 * t), int(60 - 20 * t))
        row += bytes(c)
    rows.append(bytes(row))
raw = b"".join(rows)
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
open(sys.argv[1], "wb").write(png)
EOF
  picture=$work/picture.png
fi

mkdir -p "$out"
"$work/ingame_preview" "$pcsx2/bin/resources/fonts" "$fe/assets" "$picture" "$work/data" "$out" ${FE_SIZE:-}
n=$(ls "$out"/*.png | wc -l)
echo "ingame menu: $n pictures in $out"
[[ $n -ge 40 ]] || { echo "FAIL: too few pictures"; exit 1; }
echo "PASS"
