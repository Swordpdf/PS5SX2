#!/usr/bin/env bash
# PS5SX2 (from PR #34 by Heyde Moura; AI-assisted port): builds test_discdrive.cpp as a payload and runs it on a console
# through its ELF loader (port 9021), with a PS2 disc in a USB DVD or BD drive (or the PS5's own).
#
#   PS5_PAYLOAD_SDK=~/opt/ps5-payload-sdk ps5/coreorbis/tests/discdrive/test-discdrive.sh 192.168.0.77
#
# Needs the payload SDK (a stock one is enough here) and socat. DISCDRIVE_BUILD_ONLY=1: build only (no console needed).
# DISCDRIVE_DEVICE=/dev/cd1: test that drive (default: the first /dev/cdN).
#
# Copyright (C) 2026 Heyde Moura
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

build_only=${DISCDRIVE_BUILD_ONLY:-0}
host=${1:-}
[[ $build_only == 1 || -n $host ]] || { echo "usage: test-discdrive.sh <console address> [port]" >&2; exit 2; }
port=${2:-9021}
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../../.." && pwd)
sdk=${PS5_PAYLOAD_SDK:?set PS5_PAYLOAD_SDK}
obj=${DISCDRIVE_OBJ:-$(mktemp -d)}
mkdir -p "$obj"

flags=(-O1 -g -Wall -Wno-unused-function -I"$root/ps5/coreorbis/include-orbis" -I"$root/ps5/coreorbis/orbis-shims" -I"$root" -I"$root/pcsx2" -I"$root/ps5/frontend"
  -I"$root/3rdparty/fmt/include" -I"$root/3rdparty/libchdr/include" -I"$root/ps5/third_party/lz4" -I"$root/ps5/third_party/zlib")
if [[ -n ${DISCDRIVE_DEVICE:-} ]]; then flags+=(-DDISCDRIVE_DEVICE="\"$DISCDRIVE_DEVICE\""); fi
objs=()
# PCSX2's disc reader and its thread as the app builds them (Makefile.vk's flags that matter here), and fmt for its messages.
for src in "$here/test_discdrive.cpp" "$root/ps5/coreorbis/orbis-shims/OrbisDiscDrive.cpp" \
  "$root/ps5/coreorbis/orbis-shims/OrbisIOCtlSrc.cpp" "$root/ps5/frontend/fe_games.cpp" \
  "$root/pcsx2/CDVD/CDVDdiscReader.cpp" "$root/pcsx2/CDVD/CDVDdiscThread.cpp" "$root/3rdparty/fmt/src/format.cc"; do
  o="$obj/$(basename "${src%.*}").o"
  "$sdk/bin/prospero-clang++" -std=c++20 "${flags[@]}" -D_M_X86=1 -D__POSIX__=1 -Wno-macro-redefined -include common/Threading.h \
    -I"$root/3rdparty/include" -I"$root/3rdparty/fast_float/include" -c "$src" -o "$o"
  objs+=("$o")
done
# fe_games.cpp's CHD, CSO and ZSO readers: the PC harness's CHD stand-in, the vendored LZ4 and inflate.
for c in "$root/ps5/frontend/host/chd_stub.c" "$root/ps5/third_party/lz4/lz4.c" "$root/ps5/third_party/zlib/"{adler32,crc32,inffast,inflate,inftrees,zutil}.c; do
  o="$obj/c_$(basename "$c" .c).o"
  "$sdk/bin/prospero-clang" -O1 -w -I"$root/3rdparty/libchdr/include" -I"$root/ps5/third_party/zlib" -c "$c" -o "$o"
  objs+=("$o")
done
"$sdk/bin/prospero-clang++" -o "$obj/test_discdrive.elf" "${objs[@]}" -lkernel_sys
if [[ $build_only == 1 ]]; then echo "built $obj/test_discdrive.elf (DISCDRIVE_BUILD_ONLY)"; exit 0; fi
echo "built $obj/test_discdrive.elf; running it on $host:$port"
out=$(timeout 180 socat -t 120 - "TCP:$host:$port" < "$obj/test_discdrive.elf")
echo "$out"
grep -q "^ALL PASSED" <<<"$out"
