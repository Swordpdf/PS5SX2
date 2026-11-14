#!/usr/bin/env bash
# PS5SX2 (2026-11-14, AI-assisted): PC test of the Lossless Scaling frame generation port (orbis-shims/lsfg): the DLL reader on
# synthetic PE files and the whole chain on lavapipe with stand-in shaders (test_lsfg.cpp says what it checks). No Lossless.dll
# is used: the shaders are this folder's stubs, compiled here.
#   ps5/coreorbis/tests/lsfg/test-lsfg.sh       (needs glslangValidator, lavapipe: mesa-vulkan-drivers)
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
coreorbis=$(cd -- "$here/../.." && pwd)
repo=$(cd -- "$coreorbis/../.." && pwd)
lsfg="$coreorbis/orbis-shims/lsfg"
obj=$(mktemp -d)
trap 'rm -rf "$obj"' EXIT
CXX=${CXX:-g++}
export VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json}

glslangValidator -V --target-env vulkan1.1 -o "$obj/stub.spv" "$here/stub.comp" > /dev/null
glslangValidator -V --target-env vulkan1.1 -o "$obj/stub_generate.spv" "$here/stub_generate.comp" > /dev/null

libvulkan=-lvulkan # the loader: libvulkan-dev's link, else the runtime's file itself
for f in /usr/lib/x86_64-linux-gnu/libvulkan.so /usr/lib/x86_64-linux-gnu/libvulkan.so.1; do
  if [ -e "$f" ]; then libvulkan=$f; break; fi
done
srcs=("$here/test_lsfg.cpp" "$lsfg"/*.cpp)
flags=(-std=c++20 -O1 -g -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function
  -Wno-nullability-completeness -Wno-implicit-fallthrough -Wno-parentheses -Wno-type-limits -Wno-class-memaccess
  -I"$lsfg" -I"$repo/3rdparty/vulkan/include" -pthread)
if ! "$CXX" "${flags[@]}" -fsanitize=address,undefined "${srcs[@]}" "$libvulkan" -o "$obj/test_lsfg" 2> "$obj/build.log"; then
  "$CXX" "${flags[@]}" "${srcs[@]}" "$libvulkan" -o "$obj/test_lsfg" 2> "$obj/build.log" || { cat "$obj/build.log"; exit 1; }
fi
# lavapipe's own allocations are not ours to check
# LSFG_DLL=<your own Lossless.dll> also runs Lossless Scaling's real shaders on a moving square (the generated square must be
# halfway); LSFG_OUT=<folder> keeps two of the generated pictures there (PPM). The DLL is only read.
ASAN_OPTIONS=detect_leaks=0 "$obj/test_lsfg" "$obj/stub.spv" "$obj/stub_generate.spv" "$obj" "${LSFG_DLL:-}" "${LSFG_OUT:-$obj}"
