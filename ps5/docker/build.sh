#!/usr/bin/env bash
# Builds the image (ps5/docker/Dockerfile), then compiles the port in it (Makefile.vk; extra arguments go to make).
# With PS5_VULKAN_DIR set to a built driver (ps5/README.md), it also links: ps5/build-vk/app/eboot.bin.
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)
image=${PS5SX2_IMAGE:-ps5sx2-build}

docker build -t "$image" "$here"
run=(docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$root:/src")
"${run[@]}" "$image" make -f Makefile.vk -j"$(nproc)" "$@"
if [[ -n ${PS5_VULKAN_DIR:-} ]]; then
  "${run[@]}" -v "$(realpath "$PS5_VULKAN_DIR"):/vk:ro" -e PS5_VULKAN_DIR=/vk -e ORBIS_BUILD_TAG -e ORBIS_TEST_BUILD \
    "$image" bash link-vk.sh
else
  echo "Compiled. Not linked: set PS5_VULKAN_DIR to a built driver (ps5/README.md) to link."
fi
