#!/usr/bin/env bash
# PS5SX2 (vk-285-180, AI-assisted): builds and runs the PC test of the in-app updater (frontend/fe_update.cpp) and of the PS5SX2
# Installer's launch block as the app fills it (orbis-shims/OrbisInstallerLaunch.h, against ps5/installer/src/launch.h).
#   ps5/coreorbis/tests/updater/test-updater.sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../../.." && pwd)
obj=$(mktemp -d)
trap 'rm -rf "$obj"' EXIT
CXX=${CXX:-g++}
inc=(-I"$root/ps5/coreorbis/orbis-shims" -I"$root/ps5/frontend" -I"$root/ps5/installer/src")
srcs=("$here/test_updater.cpp" "$root/ps5/frontend/fe_update.cpp")
"$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined "${inc[@]}" "${srcs[@]}" -o "$obj/test_updater" ||
  "$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread "${inc[@]}" "${srcs[@]}" -o "$obj/test_updater"
"$obj/test_updater" | tee "$obj/out.txt"
grep -q "^ALL PASSED" "$obj/out.txt"
