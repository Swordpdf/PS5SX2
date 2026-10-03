#!/usr/bin/env bash
# PS5SX2 account panel and simulated native transport checks (AI-assisted).
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
cxx=${CXX:-clang++}
flags=(-std=c++20 -O1 -g -fsanitize=undefined -ffunction-sections -fdata-sections
  -msse4.1 -I"$root" -I"$root/pcsx2" -I"$root/3rdparty/fmt/include"
  -I"$root/3rdparty/fast_float/include" -I"$root/3rdparty/simpleini/include")
"$cxx" "${flags[@]}" "$here/test_transport.cpp" \
  "$root/ps5/coreorbis/orbis-shims/ProsperoHTTPDownloader.cpp" \
  "$root/common/HTTPDownloader.cpp" "$root/common/Timer.cpp" \
  -Wl,--gc-sections -lpthread -o "$work/transport"
"$cxx" "${flags[@]}" "$root/ps5/frontend/host/achievement_panel_test.cpp" -o "$work/panel"
"$cxx" "${flags[@]}" -DPS5SX2_ACHIEVEMENTS=1 -I"$root/3rdparty/include" \
  -I"$root/3rdparty/imgui/include" "$here/test_notifications.cpp" \
  "$root/ps5/coreorbis/orbis-shims/ProsperoUI.cpp" "$root/common/SmallString.cpp" \
  -Wl,--gc-sections -lpthread -o "$work/notifications"
"$cxx" "${flags[@]}" "$here/test_account.cpp" \
  "$root/ps5/coreorbis/orbis-shims/ProsperoAchievements.cpp" \
  "$root/pcsx2/INISettingsInterface.cpp" "$root/common/MemorySettingsInterface.cpp" \
  "$root/common/Error.cpp" "$root/common/SmallString.cpp" "$root/common/StringUtil.cpp" \
  "$root/3rdparty/fmt/src/format.cc" -Wl,--gc-sections -lpthread -o "$work/account"
"$work/transport"
"$work/panel"
"$work/account" "$work/account-data"
"$work/notifications"
echo "PASS: native transport, controller account panel, credential persistence and notification adapter"
