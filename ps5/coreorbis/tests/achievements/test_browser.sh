#!/usr/bin/env bash
# Read-only browser integration checks with real rcheevos (AI-assisted).
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
cxx=${CXX:-clang++}
cc=${CC:-clang}
rc="$root/3rdparty/rcheevos"
flags=(-O1 -g -fsanitize=undefined -ffunction-sections -fdata-sections -I"$rc/include")
objects=()
for source in "$rc"/src/*.c "$rc"/src/rapi/*.c "$rc"/src/rcheevos/*.c "$rc"/src/rhash/*.c; do
  case "$source" in */rc_libretro.c|*/rc_client_raintegration.c|*/rhash/hash.c|*/rhash/cdreader.c) continue;; esac
  object="$work/$(basename "${source%.c}").o"
  # Vendored RC_OFFSETOF intentionally computes a member offset through a null
  # pointer. Suppress its null/alignment/object-size C diagnostics; keep all checks in our C++.
  "$cc" "${flags[@]}" -fno-sanitize=null,alignment,object-size -DRC_NO_THREADS=1 -DRC_HASH_NO_DISC -DRC_HASH_NO_ENCRYPTED -DRC_HASH_NO_ROM -DRC_HASH_NO_ZIP -c "$source" -o "$object"
  objects+=("$object")
done
"$cc" -O1 -I"$root/3rdparty/libchdr/include" -c "$root/ps5/frontend/host/chd_stub.c" -o "$work/chd.o"
"$cc" -O1 -c "$root/ps5/third_party/lz4/lz4.c" -o "$work/lz4.o"
"$cxx" "${flags[@]}" -std=c++20 -msse4.1 -I"$root" -I"$root/pcsx2" \
  -I"$root/3rdparty/fmt/include" -I"$root/3rdparty/fast_float/include" \
  -I"$root/3rdparty/libchdr/include" -I"$root/ps5/third_party/lz4" \
  "$here/test_browser.cpp" "$root/ps5/coreorbis/orbis-shims/ProsperoAchievementBrowser.cpp" \
  "$root/ps5/frontend/fe_games.cpp" "$root/common/HTTPDownloader.cpp" \
  "$root/common/MD5Digest.cpp" "$root/common/MemorySettingsInterface.cpp" \
  "$root/common/Timer.cpp" "$root/common/SmallString.cpp" "$root/common/StringUtil.cpp" \
  "$root/common/Error.cpp" "$root/3rdparty/fmt/src/format.cc" \
  "${objects[@]}" "$work/chd.o" "$work/lz4.o" -Wl,--gc-sections -lpthread -lz -o "$work/browser"
mkdir "$work/data"
UBSAN_OPTIONS=halt_on_error=1 "$work/browser" "$work/data"
echo "PASS: synthetic disc hash, real rcheevos read-only listing, softcore/hardcore unlocks, badge cache and failure cleanup"
