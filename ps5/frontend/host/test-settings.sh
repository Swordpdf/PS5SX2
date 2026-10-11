#!/usr/bin/env bash
# PS5 port frontend: checks that the settings page and the shelf's options sheet write the same settings files
# (settings_test.cpp), that this tree's page answers as an older one did, and (vk-285-116) that the sheet lists the page's
# options (options_dump.cpp against options_parity.js, which needs node).
#
#   ps5/frontend/host/test-settings.sh [<older commit, default 5e1e47b (1.50)>]
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fe=$(cd -- "$here/.." && pwd)
pcsx2=$(cd -- "$fe/../.." && pwd)
old_rev=${1:-5e1e47b}
work=${FE_TEST_DIR:-$here/obj/settings-test}
CXX=${CXX:-clang++-18}
CC=${CC:-clang-18}
rm -rf "$work"
mkdir -p "$work/old" "$work/common"

flags=(-O1 -g -Wall -Wno-unused-function -I"$fe" -I"$pcsx2/3rdparty/vulkan/include" -I"$pcsx2/3rdparty/libchdr/include"
  -I"$pcsx2/ps5/third_party/lz4")
common=()
for src in fe_games fe_covers fe_text fe_i18n; do
  "$CXX" -std=c++20 "${flags[@]}" -c "$fe/$src.cpp" -o "$work/common/$src.o"
  common+=("$work/common/$src.o")
done
"$CC" -O1 -c "$pcsx2/ps5/third_party/lz4/lz4.c" -o "$work/common/lz4.o"
"$CC" -O1 -I"$pcsx2/3rdparty/libchdr/include" -c "$here/chd_stub.c" -o "$work/common/chd_stub.o"
common+=("$work/common/lz4.o" "$work/common/chd_stub.o")

# This tree: the page (fe_web.cpp over fe_settings.cpp) and the sheet's calls.
"$CXX" -std=c++20 "${flags[@]}" -DFE_HAVE_SETTINGS "$here/settings_test.cpp" "$fe/fe_web.cpp" "$fe/fe_settings.cpp" "${common[@]}" \
  -o "$work/settings_test_new" -lz -lpthread
# The older page, as it was built then.
git -C "$pcsx2" show "$old_rev:ps5/frontend/fe_web.cpp" > "$work/old/fe_web.cpp"
"$CXX" -std=c++20 "${flags[@]}" "$here/settings_test.cpp" "$work/old/fe_web.cpp" "${common[@]}" -o "$work/settings_test_old" -lz -lpthread

presets="$fe/assets/presets.ini"
"$work/settings_test_new" "$work/new" web "$presets" > "$work/web-new.txt" 2> "$work/web-new.log"
"$work/settings_test_old" "$work/old" web "$presets" > "$work/web-old.txt" 2> "$work/web-old.log"
"$work/settings_test_new" "$work/new" sheet "$presets" > "$work/sheet.txt" 2> "$work/sheet.log"

fail=0
# The server's own "[web] ..." lines (port, token, paths, times) differ from run to run: left out.
grep -v '^\[web\] ' "$work/web-old.txt" > "$work/web-old.cmp" || true
grep -v '^\[web\] ' "$work/web-new.txt" > "$work/web-new.cmp" || true
if diff -u "$work/web-old.cmp" "$work/web-new.cmp" > "$work/web.diff"; then
  echo "PASS: the settings page answers and writes as at $old_rev ($(grep -c '^>>>' "$work/web-new.txt") requests)"
else
  echo "FAIL: the settings page differs from $old_rev (see $work/web.diff)"; fail=1
fi
# The page's files with their comment lines left out, against the sheet's.
awk '/^=== /{on=1} on && !/^#/' "$work/web-new.txt" > "$work/web-files.txt"
awk '/^=== /{on=1} on' "$work/sheet.txt" > "$work/sheet-files.txt"
if diff -u "$work/web-files.txt" "$work/sheet-files.txt" > "$work/files.diff"; then
  echo "PASS: the sheet writes the same files as the page ($(grep -c '^=== ' "$work/sheet-files.txt") files)"
else
  echo "FAIL: the sheet's files differ from the page's (see $work/files.diff)"; fail=1
fi
# vk-285-121: a console without settings/: both writers make it.
if "$work/settings_test_new" "$work/new" nosettings "$presets" > "$work/nosettings.txt" 2> "$work/nosettings.log"; then
  echo "PASS: the sheet and the page make a missing settings/ ($(grep -c '^PASS' "$work/nosettings.txt") saves)"
else
  echo "FAIL: a save into a missing settings/ (see $work/nosettings.txt)"; fail=1
fi
# vk-285-116: the sheet's options against the page's GROUPS (order, tabs, keys, labels, defaults, values).
"$CXX" -std=c++20 "${flags[@]}" "$here/options_dump.cpp" "$fe/fe_options.cpp" "$fe/fe_settings.cpp" "${common[@]}" \
  -o "$work/options_dump" -lz -lpthread
"$work/options_dump" > "$work/options-sheet.txt"
if command -v node > /dev/null; then
  node "$here/options_parity.js" "$fe/assets/web/index.html" > "$work/options-page.txt"
  if diff -u "$work/options-page.txt" "$work/options-sheet.txt" > "$work/options.diff"; then
    echo "PASS: the sheet lists the page's options ($(grep -c '^I ' "$work/options-sheet.txt") options in $(grep -c '^G ' "$work/options-sheet.txt") groups)"
  else
    echo "FAIL: the sheet's options differ from the page's (see $work/options.diff)"; fail=1
  fi
else
  echo "SKIP: no node, the sheet's options weren't checked against the page's"
fi
head -n 1 "$work/sheet.txt"
exit $fail
