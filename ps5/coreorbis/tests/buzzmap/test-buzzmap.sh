#!/usr/bin/env bash
# PS5 port: the Buzz! buzzers' buttons (orbis-shims/OrbisBuzzMap.h, PR #91) on a PC. 2.41 (AI-assisted).
#   ps5/coreorbis/tests/buzzmap/test-buzzmap.sh
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
CXX=${CXX:-clang++-18}
"$CXX" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$here/../../orbis-shims" "$here/test_buzz_map.cpp" -o "$work/test_buzz_map"
"$work/test_buzz_map"

# 2.41: main-boot.cpp's OrbisBuzzPump sends binds 0..19 in OrbisBuzzMap.h's colour order (red, blue, orange, green, yellow; player by player)
# without usb-buzz.h (it needs the qemu USB headers): its enum must still be that.
want=""
for p in 1 2 3 4; do for c in RED BLUE ORANGE GREEN YELLOW; do want+="CID_BUZZ_PLAYER${p}_${c} "; done; done
want+="CID_BUZZ_COUNT "
got=$(sed -n '/enum BuzzControlID/,/};/p' "$here/../../../../pcsx2/USB/usb-pad/usb-buzz.h" | grep -o 'CID_BUZZ_[A-Z0-9_]*' | tr '\n' ' ')
if [ "$got" != "$want" ]; then
	echo "FAIL  usb-buzz.h's BuzzControlID changed: main-boot.cpp's kBuzzFirstBind/kBuzzBinds and OrbisBuzzMap.h's colours must follow it"
	exit 1
fi
grep -qF 'static constexpr u32 kBuzzFirstBind = 0, kBuzzBinds = 20;' "$here/../../main-boot.cpp" || { echo "FAIL  main-boot.cpp's buzzer bind numbers changed"; exit 1; }
echo "test-buzzmap: usb-buzz.h's binds match main-boot.cpp's"
