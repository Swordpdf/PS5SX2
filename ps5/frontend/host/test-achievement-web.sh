#!/usr/bin/env bash
# In-game web API and browser behavior checks (AI-assisted).
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
cxx=${CXX:-clang++}
cc=${CC:-clang}
"$cc" -O1 -I"$root/3rdparty/libchdr/include" -c "$here/chd_stub.c" -o "$work/chd.o"
"$cc" -O1 -c "$root/ps5/third_party/lz4/lz4.c" -o "$work/lz4.o"
"$cxx" -std=c++20 -O1 -g -fsanitize=undefined -ffunction-sections -fdata-sections \
  -I"$root/3rdparty/libchdr/include" -I"$root/ps5/third_party/lz4" \
  -I"$root/3rdparty/vulkan/include" \
  "$here/achievement_web_test.cpp" "$root/ps5/frontend/fe_web.cpp" \
  "$root/ps5/frontend/fe_settings.cpp" "$root/ps5/frontend/fe_games.cpp" \
  "$root/ps5/frontend/fe_covers.cpp" "$root/ps5/frontend/fe_text.cpp" "$root/ps5/frontend/fe_i18n.cpp" \
  "$work/chd.o" "$work/lz4.o" -Wl,--gc-sections -lz -lpthread -o "$work/web-test"
UBSAN_OPTIONS=halt_on_error=1 "$work/web-test" "$work"
node "$here/achievement_web_test.js" "$root/ps5/frontend/assets/web/index.html"
echo "PASS: in-game achievement API access control, live updates, image validation and browser filters/pagination"
