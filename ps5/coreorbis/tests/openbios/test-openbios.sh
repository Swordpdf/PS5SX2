#!/usr/bin/env bash
# PS5 port: the BIOS before every start and the OpenPS2BIOS PS5SX2 ships (orbis-shims/OrbisOpenBios.h), with PCSX2's own BIOS reading
# (LoadBiosVersion, cut out of pcsx2/ps2/BiosTools.cpp) run on the shipped image. bioscheck, 2026-10-29 (AI-assisted).
#   ps5/coreorbis/tests/openbios/test-openbios.sh
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../../.." && pwd)
shims="$root/ps5/coreorbis/orbis-shims"
bin="$root/ps5/frontend/assets/openps2bios/OpenPS2BIOS-0.5.bin"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
CXX=${CXX:-clang++-18}

# The shipped image is the one the owner verified (OpenPS2BIOS v0.5's release file).
want=e6d3b308ba10a8603bad929f59e50f8c023acc2c297aaa82ec0bd155e99b98fc
got=$(sha256sum "$bin" | cut -d' ' -f1)
if [ "$got" != "$want" ]; then
	echo "FAIL  $bin is not OpenPS2BIOS 0.5's release file ($got)"
	exit 1
fi
echo "PASS  the shipped image is OpenPS2BIOS 0.5 (sha256 $want)"
test -s "$root/ps5/frontend/assets/openps2bios/LICENSE" && grep -q "MIT License" "$root/ps5/frontend/assets/openps2bios/LICENSE" &&
	echo "PASS  its MIT licence is beside it" || { echo "FAIL  no LICENSE beside the image"; exit 1; }

# PCSX2's romdir struct and LoadBiosVersion, as they are in the tree.
python3 - "$root/pcsx2/ps2/BiosTools.cpp" "$work/bios_parse.inc" <<'PY'
import sys
src = open(sys.argv[1]).read()
a = src.index("#pragma pack(push, 1)")
b = src.index("#pragma pack(pop)") + len("#pragma pack(pop)")
c = src.index("static bool LoadBiosVersion(")
d = src.index("static void ChecksumIt(")
open(sys.argv[2], "w").write(src[a:b] + "\n\n" + src[c:d])
PY

flags=(-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -I"$shims" -I"$work")
"$CXX" "${flags[@]}" -D__PROSPERO__ "$here/test_openbios.cpp" -o "$work/test_ps5"
"$CXX" "${flags[@]}" -DOB_UPSTREAM "$here/test_openbios.cpp" -o "$work/test_upstream"
echo "-- as the PS5 build reads it"
mkdir -p "$work/a" "$work/b"
"$work/test_ps5" "$bin" "$work/a"
echo "-- as upstream PCSX2 reads it"
"$work/test_upstream" "$bin" "$work/b"

# main-boot.cpp cannot be built on a PC: the gate's glue is pinned by its text (every start path goes through it).
main="$root/ps5/coreorbis/main-boot.cpp"
missing=0
pin() {
	if ! grep -qF -- "$1" "$main"; then
		echo "FAIL  main-boot.cpp no longer has: $1"
		missing=1
	fi
}
pin 'pre_from = OrbisBios::StartFrom::GameTile;'
pin 'pre_from = OrbisBios::StartFrom::DiscTile;'
pin 'pre_from = OrbisBios::StartFrom::DiscLaunch;'
pin 'pre_from = OrbisBios::StartFrom::BootDisc;'
pin 'orbis_bios_gate(frontend_ran ? OrbisBios::StartFrom::Shelf : OrbisBios::StartFrom::PlainList'
pin 'fe.bios_check = []() { return orbis_bios_usable(); };'
pin 'orbis_bios_gate(pre_from, s_game_path, shelf_can_open)'
pin 'OrbisBios::OpenBiosCanStart('
pin 'orbis_bios_usable(); // vk-285-134'
[ $missing = 0 ] && echo "PASS  main-boot.cpp: every start path goes through orbis_bios_gate"
exit $missing
