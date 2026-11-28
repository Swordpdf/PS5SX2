#!/usr/bin/env bash
# PS5SX2 (AI-assisted): builds and runs the PC unit test of RADV's NFS directory shim (orbis-shims/radv_dirshim.cpp)
# and checks link-radv.sh wires radv_dirshim.o instead of --defsym'ing directory calls directly to ps5_*.
#   ps5/coreorbis/tests/nfs/test-radv-dirshim.sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../../.." && pwd)
obj=$(mktemp -d)
trap 'rm -rf "$obj"' EXIT

CXX=${CXX:-g++}
"$CXX" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$root/ps5/coreorbis/include-orbis" \
  "$root/ps5/coreorbis/orbis-shims/radv_dirshim.cpp" \
  "$here/test_radv_dirshim.cpp" -o "$obj/test_radv_dirshim" ||
"$CXX" -std=c++20 -O1 -g -Wall -Wextra -Werror \
  -I"$root/ps5/coreorbis/include-orbis" \
  "$root/ps5/coreorbis/orbis-shims/radv_dirshim.cpp" \
  "$here/test_radv_dirshim.cpp" -o "$obj/test_radv_dirshim"

"$obj/test_radv_dirshim" | tee "$obj/out.txt"
grep -q "^ALL PASSED" "$obj/out.txt"

link_radv="$root/ps5/coreorbis/link-radv.sh"
bash -n "$link_radv"
grep -qF 'orbis-shims/radv_dirshim.cpp' "$link_radv" ||
  { echo "FAIL: link-radv.sh does not compile orbis-shims/radv_dirshim.cpp"; exit 1; }
grep -qF 'radv_dirshim.o' "$link_radv" ||
  { echo "FAIL: link-radv.sh does not link radv_dirshim.o"; exit 1; }

# Extract bound=( ... ) and ensure opendir/readdir/rewinddir/dirfd/closedir are not --defsym'd directly.
bound_block=$(awk '/^bound=\(/,/\)/' "$link_radv")
for sym in opendir readdir rewinddir dirfd closedir; do
  if grep -qw "$sym" <<<"$bound_block"; then
    echo "FAIL: link-radv.sh still lists $sym in bound=() (would bypass OrbisNfs)"
    exit 1
  fi
done
echo "link-radv.sh regression checks: OK"
