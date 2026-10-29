#!/usr/bin/env bash
# PS5 port frontend (2026-10-29, AI-assisted): an HD pack's download + unpack under the console's write limits for a title (write_model.c by
# LD_PRELOAD: a full-speed burst, then 2 MiB/s, 3.3 ms a call), written by the app itself (flag texpack_titlewrites) and by the writer process
# (fe_texwrite.h, the payload built for the PC). The burst is scaled down with the pack (WM_BURST_MB, default 32 MiB for a 96 MiB pack, as 1.3 GiB
# is for a ~4 GB pack); the rest are the console's numbers. A model: the console's own numbers come from its session reports.
#
#   ps5/frontend/host/texwrite-bench.sh            BENCH_MB=96 WM_BURST_MB=32 WM_RATE_MBS=2 WM_CALL_US=3300
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fe=$(cd -- "$here/.." && pwd)
pcsx2=$(cd -- "$fe/../.." && pwd)
la="$pcsx2/ps5/third_party/libarchive"
CXX=${CXX:-clang++-18}
CC=${CC:-clang-18}
work=$(mktemp -d "${TMPDIR:-/tmp}/twbench.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/obj"
objs=()
for c in "$la"/libarchive/*.c; do
  o="$work/obj/$(basename "$c" .c).o"
  "$CC" -O2 -w -DHAVE_CONFIG_H -I"$la" -I"$la/libarchive" -c "$c" -o "$o"
  objs+=("$o")
done
"$CC" -O2 -c "$pcsx2/ps5/third_party/lz4/lz4.c" -o "$work/obj/lz4.o"
"$CC" -O2 -I"$pcsx2/3rdparty/libchdr/include" -c "$here/chd_stub.c" -o "$work/obj/chd_stub.o"
sdk="$pcsx2/3rdparty/lzma"
for s in 7zArcIn 7zBuf 7zCrc 7zCrcOpt 7zDec 7zStream Bcj2 Bra Bra86 BraIA64 CpuArch Delta Lzma2Dec LzmaDec; do
  "$CC" -O2 -w -DZ7_ST -I"$sdk/include" -c "$sdk/src/$s.c" -o "$work/obj/sz_$s.o"
  objs+=("$work/obj/sz_$s.o")
done
"$CC" -O2 -w -DZ7_ST -I"$sdk/include" -c "$fe/fe_sevenzip.c" -o "$work/obj/fe_sevenzip.o"
objs+=("$work/obj/fe_sevenzip.o")
"$CXX" -std=c++20 -O2 -Wall -Wno-unused-function -I"$fe" -I"$pcsx2/pcsx2" -I"$la/libarchive" -I"$pcsx2/3rdparty/rapidjson/include" \
  -I"$pcsx2/3rdparty/vulkan/include" -I"$pcsx2/3rdparty/libchdr/include" -I"$pcsx2/ps5/third_party/lz4" \
  "$here/texwrite_bench.cpp" "$fe/fe_texpacks.cpp" "$fe/fe_texwrite.cpp" "$fe/fe_settings.cpp" "$fe/fe_games.cpp" "$fe/fe_covers.cpp" "$fe/fe_text.cpp" \
  "$fe/fe_i18n.cpp" "${objs[@]}" "$work/obj/lz4.o" "$work/obj/chd_stub.o" -lz -lpthread -o "$work/bench"
"$CC" -O2 -Wall -I"$fe" "$pcsx2/ps5/coreorbis/tex-writer/tex_writer.c" -o "$work/tex_writer"
"$CC" -shared -fPIC -O1 -Wall -o "$work/write_model.so" "$here/write_model.c" -ldl

mb=${BENCH_MB:-96}
python3 - "$work/pack" "$mb" <<'EOF'
import os, sys, zipfile
d, mb = sys.argv[1], int(sys.argv[2])
os.makedirs(d)
with zipfile.ZipFile(os.path.join(d, "Bench (USA) [SLUS-12121] HD Remaster.zip"), "w", zipfile.ZIP_STORED) as z:
    for i in range(mb):
        z.writestr("Bench/SLUS-12121/replacements/%03d.dds" % i, os.urandom(1 << 20))
EOF
export WM_BURST_MB=${WM_BURST_MB:-32} WM_RATE_MBS=${WM_RATE_MBS:-2} WM_CALL_US=${WM_CALL_US:-3300}
echo "model: ${WM_BURST_MB} MiB at full speed, then ${WM_RATE_MBS} MiB/s, ${WM_CALL_US} us a write call; a ${mb} MiB pack (stored zip)"
for mode in titlewrites writer; do
  rm -rf "$work/run-$mode"
  WM_REPORT="$work/model-$mode.txt" TEX_WRITER_BIN="$work/tex_writer" LD_PRELOAD="$work/write_model.so" \
    "$work/bench" "$work/pack/Bench (USA) [SLUS-12121] HD Remaster.zip" SLUS-12121 "$work/run-$mode" "$mode" > "$work/log-$mode.txt" 2>&1 || true
  grep -E "BENCH|downloaded|unpacked:|writer process|replacements.pak:" "$work/log-$mode.txt" | sed 's/^/  /'
  sed 's/^/  /' "$work/model-$mode.txt"
done
