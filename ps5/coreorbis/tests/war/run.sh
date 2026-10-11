#!/usr/bin/env bash
# PS5SX2 (vk-285-119, AI-assisted): runs test_war.c -- orbis-shims/orbis_ps5vk_war.c against the driver release's own
# ps5vk_cmd_buffer.o, renamed as link-vk.sh renames it -- on the PC.
#   PS5_VULKAN_DIR=<the 6a20943 driver release folder> tests/war/run.sh [steps]
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
shims=$(cd -- "$here/../../orbis-shims" && pwd)
VK=${PS5_VULKAN_DIR:?set PS5_VULKAN_DIR to the 6a20943 driver release folder}
WANT=885012e2c56712e5646212e3b3cb1c6a1794535ccefbb40b57dbd234cac61019 # ps5vk_cmd_buffer.o in that release
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
(cd "$work" && ar x "$VK/lib/libps5vk.ps5.a" ps5vk_cmd_buffer.o)
got=$(sha256sum "$work/ps5vk_cmd_buffer.o" | cut -d' ' -f1)
[[ $got == "$WANT" ]] || { echo "ps5vk_cmd_buffer.o is $got, not the 6a20943 release's: the shim's offsets are for that one" >&2; exit 1; }
OBJCOPY=$(command -v llvm-objcopy || command -v objcopy)
"$OBJCOPY" --redefine-sym ps5vk_cmd_buffer_note_draw_samples=ps5vk_cmd_buffer_note_draw_samples_orig \
  --redefine-sym ps5vk_cmd_buffer_sampled_earlier=ps5vk_cmd_buffer_sampled_earlier_orig "$work/ps5vk_cmd_buffer.o"
CC=${CC:-clang}
"$CC" -std=c11 -O2 -mavx2 -Wall -Wextra -o "$work/test_war" "$here/test_war.c" "$shims/orbis_ps5vk_war.c" \
  "$work/ps5vk_cmd_buffer.o" -Wl,--gc-sections -Wl,--unresolved-symbols=ignore-all
"$work/test_war" "$@"
