#!/usr/bin/env bash
# Builds the RADV image (Dockerfile.radv, over Dockerfile's), compiles the port and links it with RADV:
# ps5/build-radv/app/eboot.bin.
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)
base=${PS5SX2_IMAGE:-ps5sx2-build}
image=${PS5SX2_RADV_IMAGE:-ps5sx2-build-radv}

docker build -t "$base" "$here"
docker build -t "$image" -f "$here/Dockerfile.radv" --build-arg BASE="$base" "$here"
run=(docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$root:/src" -e ORBIS_BUILD_TAG -e ORBIS_TEST_BUILD "$image")
"${run[@]}" make -f Makefile.vk -j"$(nproc)"
"${run[@]}" bash link-radv.sh
