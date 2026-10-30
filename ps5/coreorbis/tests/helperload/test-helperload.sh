#!/usr/bin/env bash
# PS5SX2 (vk-285-179, AI-assisted): builds and runs the PC test of the helper auto-load decision (OrbisHelperAutoload.h).
#   ps5/coreorbis/tests/helperload/test-helperload.sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../../.." && pwd)
obj=$(mktemp -d)
trap 'rm -rf "$obj"' EXIT
CXX=${CXX:-g++}
"$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I"$root/ps5/coreorbis/orbis-shims" "$here/test_helperload.cpp" -o "$obj/test_helperload" ||
  "$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror -I"$root/ps5/coreorbis/orbis-shims" "$here/test_helperload.cpp" -o "$obj/test_helperload"
"$obj/test_helperload" | tee "$obj/out.txt"
grep -q "^ALL PASSED" "$obj/out.txt"
# fix180: the auto-load and the installer's start run after the jailbreak, when /app0 is no longer the app's folder (every vk-285-180 report: "no helper
# ELF shipped with the app"): the app's folder must also be looked at by its /data path.
shims="$root/ps5/coreorbis/orbis-shims"
grep -qF '"/data/homebrew/PPSA99203/PS5SXHelper.elf",' "$shims/OrbisHelperAutoload.cpp" ||
  { echo "FAIL: OrbisHelperAutoload.cpp does not look for the helper in /data/homebrew/PPSA99203"; exit 1; }
grep -qF '"/data/homebrew/PPSA99203/PS5SX2Installer.elf"' "$shims/OrbisInstallerStart.cpp" ||
  { echo "FAIL: OrbisInstallerStart.cpp does not look for the installer in /data/homebrew/PPSA99203"; exit 1; }
echo "shipped ELF paths: /data/homebrew/PPSA99203 looked at (after the jailbreak /app0 is not the app's folder)"
