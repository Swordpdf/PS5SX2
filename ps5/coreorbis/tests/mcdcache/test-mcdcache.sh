#!/usr/bin/env bash
# Exercise the actual cached memory-card flush with libc streams and recoverable I/O failures. (AI-assisted)
# The test isolates FlushCache from PCSX2; it does not test the complete emulator or a physical memory card.
# CXX=clang++ MCDCACHE_SOURCE=/path/to/MemoryCardFile.cpp ./test-mcdcache.sh
# SPDX-License-Identifier: GPL-3.0+
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../../.." && pwd)
mcd_source=${MCDCACHE_SOURCE:-$root/pcsx2/SIO/Memcard/MemoryCardFile.cpp}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
CXX=${CXX:-clang++-18}

python3 - "$mcd_source" "$work/mcdcache_actual.inc" "$work/mcdcache_size.inc" <<'PY'
import re, sys
from pathlib import Path
source = Path(sys.argv[1]).read_text()
start = source.index("void FileMemoryCard::FlushCache(")
end = source.index("\n}\n", start) + 3
Path(sys.argv[2]).write_text(source[start:end])
size = re.search(r"^static constexpr int MC2_ERASE_SIZE = .*;$", source, re.M)
if not size:
    raise SystemExit("MC2_ERASE_SIZE not found")
Path(sys.argv[3]).write_text(size.group(0) + "\n")
PY

"$CXX" -std=c++20 -O1 -g -Wall -Wextra -Werror -I"$work" "$here/mcdcache_test.cpp" -o "$work/mcdcache_test"
"$work/mcdcache_test"
