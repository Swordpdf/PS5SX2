#!/usr/bin/env bash
# PS5SX2 (2.60, AI-assisted): builds PS5SX2's RADV: Mihawk-99's PS5 Mesa (github.com/mihawk-99/PS5_Mesa, main at 75238ae4db3) with
# ps5sx2-radv.patch, the way PS5_Vulkan's tools/build-radv.sh builds a release (its meson options, its cross file, no assertions), into
# $OUT/lib/libvulkan_radeon.ps5.a with $OUT/PROVENANCE.txt. ps5/coreorbis/link-radv.sh links it (PS5_RADV_ARCHIVE).
#
#   PS5_MESA=<a PS5_Mesa clone> PS5_VULKAN=<a PS5_Vulkan clone at 7f492b4, after its tools/setup-native-dependencies.sh> \
#     OUT=<folder> ps5/radv/build-radv.sh
#
# The clone of PS5_Mesa is only read (git archive of the revision); the patched tree, the host tools and the build are in $OUT.
# Needs meson, ninja, rsync, the host's LLVM with libclc and the SPIR-V translator (for Mesa's mesa_clc, as PS5_Vulkan's script), and
# clang/LLVM 18 for the console build (the payload SDK's wrapper picks the newest LLVM otherwise).
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
MESA=${PS5_MESA:?set PS5_MESA to a clone of github.com/mihawk-99/PS5_Mesa}
V=${PS5_VULKAN:?set PS5_VULKAN to a clone of github.com/mihawk-99/PS5_Vulkan at 7f492b4 (tools/setup-native-dependencies.sh run)}
OUT=${OUT:-$here/build}
REV=75238ae4db3
SDK=$V/.deps/native/ps5-payload-sdk
PATCH=$here/ps5sx2-radv.patch
log() { echo "[build-radv] $*"; }
[[ -f $SDK/.ps5-sdk-revision ]] || { echo "PS5_Vulkan's payload SDK is missing: run its tools/setup-native-dependencies.sh" >&2; exit 2; }
git -C "$MESA" cat-file -e "$REV^{commit}" 2>/dev/null || { echo "$MESA does not have $REV: git fetch it" >&2; exit 2; }
mkdir -p "$OUT"

# 1. The tree: the revision, with the patch.
src=$OUT/mesa-src
if [[ ! -f $src/.ps5sx2-patch ]] || ! cmp -s "$src/.ps5sx2-patch" "$PATCH"; then
  rm -rf "$src"
  mkdir -p "$src"
  git -C "$MESA" archive "$REV" | tar -x -C "$src"
  patch -d "$src" -p1 --quiet < "$PATCH"
  cp "$PATCH" "$src/.ps5sx2-patch"
  log "tree: PS5_Mesa $REV + ps5sx2-radv.patch"
fi
id=$( { echo "$REV"; cat "$PATCH"; } | sha1sum | cut -c1-40)

# 2. Mesa's host tools (mesa_clc and vtn_bindgen2, for the geometry shaders run as compute) with the host's default LLVM.
clc=$OUT/build-clc
if [[ ! -f $clc/build.ninja ]]; then
  env -u LLVM_CONFIG meson setup "$clc" "$src" -Dbuildtype=release -Dmesa-clc=enabled -Dinstall-mesa-clc=true \
    -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
    -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dllvm=enabled -Dshared-llvm=enabled \
    -Dbuild-tests=false -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled -Dxmlconfig=disabled -Dtools= > "$clc.setup.log" 2>&1 \
    || { grep -n ERROR "$clc.setup.log" >&2; exit 1; }
fi
env -u LLVM_CONFIG ninja -C "$clc" src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 > "$clc.log" 2>&1 \
  || { grep -n "FAILED\|error:" "$clc.log" | head >&2; exit 1; }
mkdir -p "$OUT/clc-bin"
ln -sf "$clc/src/compiler/clc/mesa_clc" "$OUT/clc-bin/mesa_clc"
ln -sf "$clc/src/compiler/spirv/vtn_bindgen2" "$OUT/clc-bin/vtn_bindgen2"
export PATH=$OUT/clc-bin:$PATH

# 3. The console build: PS5_Vulkan's release options (tools/build-radv.sh) and cross file.
export LLVM_CONFIG=${LLVM_CONFIG:-/usr/bin/llvm-config-18}
build=$OUT/build-release
constants=$OUT/cross-constants.ini
printf "[constants]\nsdk = '%s'\n" "$SDK" > "$constants"
options=(-Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms= -Dradv-winsys=ps5
  -Dllvm=disabled -Damd-use-llvm=false -Dvideo-codecs=
  -Dbuildtype=debugoptimized -Db_ndebug=true
  -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled
  -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled -Dzlib=enabled --force-fallback-for=zlib
  -Dexpat=disabled -Dxmlconfig=disabled -Dshader-cache=enabled -Dbuild-tests=false -Dvulkan-layers= -Dtools=
  -Dmesa-clc=system)
if [[ ! -f $build/build.ninja ]]; then
  meson setup "$build" "$src" --cross-file "$constants" --cross-file "$V/tooling/radv/ps5-cross.ini" "${options[@]}" \
    -Dradv-build-id="$id" > "$build.setup.log" 2>&1 || { tail -20 "$build.setup.log" >&2; exit 1; }
elif [[ $(cat "$build/.ps5sx2-build-id" 2>/dev/null) != "$id" ]]; then
  meson configure "$build" -Dradv-build-id="$id" > "$build.setup.log" 2>&1 || { tail -20 "$build.setup.log" >&2; exit 1; }
fi
echo "$id" > "$build/.ps5sx2-build-id"
log "compiling (build id $id)"
ninja -C "$build" src/amd/vulkan/libvulkan_radeon.a > "$build.log" 2>&1 || { grep -n -A6 "error:" "$build.log" | head -40 >&2; exit 1; }
mkdir -p "$OUT/lib"
cp "$build/src/amd/vulkan/libvulkan_radeon.a" "$OUT/lib/libvulkan_radeon.ps5.a"
{
  echo "RADV for PS5SX2: PS5_Mesa $REV (github.com/mihawk-99/PS5_Mesa) + ps5/radv/ps5sx2-radv.patch"
  echo "revision: $REV"
  echo "build id: $id"
  echo "assertions: off"
  echo "sdk: $(cat "$SDK/.ps5-sdk-revision")"
  echo "archive sha256: $(sha256sum "$OUT/lib/libvulkan_radeon.ps5.a" | cut -d' ' -f1)"
} > "$OUT/PROVENANCE.txt"
log "done: $OUT/lib/libvulkan_radeon.ps5.a"
