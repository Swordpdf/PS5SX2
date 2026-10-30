#!/usr/bin/env bash
# PS5 port frontend: the shelf's game scan with CHD images (chdscan, AI-assisted; a PS5 Pro tester with many CHDs on a USB hard drive: a long
# black screen before the shelf, "only with CHD games - ISO is much faster").
#   1. chdscan_test.cpp, with the real libchdr (built here) and CHD images made by make_chd_fixtures.py (sparse files: a few MB on disk):
#      what a serial costs (bytes read, against libchdr's own open and an ISO), far reads, the serial cache with images that have no serial;
#   2. fe_host (lavapipe or SwiftShader for the GPU): the shelf comes up while the scan runs, says how far it got, and gets its games after.
# Pictures go to $FE_SHOTS when it is set.
#
#   ps5/frontend/host/test-chdscan.sh
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fe=$(cd -- "$here/.." && pwd)
pcsx2=$(cd -- "$fe/../.." && pwd)
obj=${FE_HOST_OBJ:-$here/obj}/chdscan
CXX=${CXX:-clang++-18}
CC=${CC:-clang-18}
mkdir -p "$obj/chd"

# libchdr as the PS5 build has it (ps5/coreorbis/Makefile.vk CHD_CSRCS), with the machine's zlib
inc=(-I"$pcsx2/3rdparty/libchdr/include" -I"$pcsx2/3rdparty/lzma/include" -I"$pcsx2/ps5/third_party/zstd/lib")
defs=(-DZ7_ST -DZSTD_DISABLE_ASM=1 -DZSTD_LEGACY_SUPPORT=0 -DZSTD_NO_TRACE=1)
chd_objs=()
for c in "$pcsx2"/3rdparty/libchdr/src/*.c "$pcsx2/3rdparty/lzma/src/LzmaDec.c" "$pcsx2"/ps5/third_party/zstd/lib/common/*.c \
  "$pcsx2"/ps5/third_party/zstd/lib/decompress/*.c; do
  o="$obj/chd/$(basename "$(dirname "$c")")_$(basename "$c" .c).o"
  [[ -f $o && ! $c -nt $o ]] || "$CC" -O2 -w "${defs[@]}" "${inc[@]}" -c "$c" -o "$o"
  chd_objs+=("$o")
done

# the images (made again when the maker changes)
fx=$obj/fixtures
stamp=$(md5sum "$here/make_chd_fixtures.py" | cut -c1-12)
if [[ ! -f $fx/$stamp ]]; then
  rm -rf "$fx"
  mkdir -p "$fx"
  gen=$here/make_chd_fixtures.py
  python3 "$gen" "$fx/dvd2k.chd" dvd 4200 2048 SLUS_203.70 --iso "$fx/dvd.iso"
  python3 "$gen" "$fx/dvd4k.chd" dvd 4200 4096 SLUS_203.74
  python3 "$gen" "$fx/cd.chd" cd 700 19584 SLUS_203.71
  python3 "$gen" "$fx/far2k.chd" dvd 4200 2048 SLUS_203.72 --elf-lba 1500000
  python3 "$gen" "$fx/farcd.chd" cd 700 19584 SLUS_203.73 --elf-lba 200000
  python3 "$gen" "$fx/novolume.chd" cd 500 19584 SLUS_000.00 --no-volume
  touch "$fx/$stamp"
fi

flags=(-O1 -g -Wall -Wno-unused-function -I"$fe" -I"$pcsx2/3rdparty/vulkan/include" -I"$pcsx2/3rdparty/libchdr/include" -I"$pcsx2/ps5/third_party/lz4")
"$CC" -O1 -c "$pcsx2/ps5/third_party/lz4/lz4.c" -o "$obj/lz4.o"
"$CXX" -std=c++20 "${flags[@]}" "$here/chdscan_test.cpp" "$fe/fe_games.cpp" "$fe/fe_covers.cpp" "$fe/fe_text.cpp" "$fe/fe_i18n.cpp" \
  "$fe/fe_settings.cpp" "$obj/lz4.o" "${chd_objs[@]}" -o "$obj/chdscan_test" -lz -lpthread 2>&1 | grep -v "FontData\|fe_text.h\|In file included\|^ *[0-9]* |\|^ *\^\|warning generated" || true
failed=0
"$obj/chdscan_test" "$fx" "$obj/run" || failed=1
rm -rf "$obj/run"

# 2. the shelf while the scan runs
"$here/build-host.sh" >/dev/null
export VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-$(ls /usr/share/vulkan/icd.d/lvp_icd.json /opt/pw-browsers/*/chrome-linux/vk_swiftshader_icd.json 2>/dev/null | head -1)}
work=$(mktemp -d "${TMPDIR:-/tmp}/chdscan-shelf.XXXXXX")
trap 'rm -rf "$work"' EXIT
shots=${FE_SHOTS:-$work/shots}
run() {
  local name=$1
  shift
  if out=$("$here/fe_host" --data "$work/$name" --out "$shots" --size 960x540 "$@" 2>&1) && ! grep -q "expect FAILED" <<<"$out"; then
    echo "ok: $name"
  else
    echo "FAILED: $name"
    grep "expect\|\[host\]\|scan" <<<"$out" | grep -v "^\[host\] device" || true
    failed=1
  fi
}
# Up while the scan runs, no games yet, the count on screen; then the 5 games, the last one played picked, their cases drawn.
run scanning "game 2" "wait 0.3" "scan 3/40" "wait 0.2" "expect scanning=1" "expect shelf=0" "shot chdscan-scanning" "scan 27/40" "wait 0.2" \
  "scan done" "wait 0.1" "expect scanning=0" "expect shelf=5" "expect selected=2" "wait 3" "expect setat2=1" "shot chdscan-done"
# Moving on the empty shelf does nothing; the games come and the shelf works as ever.
run scan-input "scan 0/12" "press right 3" "press cross" "expect done=0" "scan done" "wait 0.2" "press right" "expect selected=1" "expect shelf=5"
# A disc in the drive while the scan runs: its entry is on the shelf first, and stays first when the games come.
run scan-drive "scan 1/9" "drive SLUS-21000 Drive Game" "wait 0.3" "expect driveentry=1" "expect shelf=1" "shot chdscan-drive" "scan done" "wait 0.2" \
  "expect shelf=6" "expect driveentry=1"
# Without a scan nothing changes: the games are there from the first frame.
run no-scan "expect shelf=5" "expect scanning=0"
if [[ $failed == 0 ]]; then
  echo "test-chdscan: all passed"
else
  echo "test-chdscan: FAILED"
  exit 1
fi
