#!/usr/bin/env bash
# PS5 port: how "back to the menu" leaves the process (orbis-shims/OrbisLeavePlan.h) on a PC. 2.11 (AI-assisted).
#   ps5/coreorbis/tests/leaveplan/test-leaveplan.sh
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
CXX=${CXX:-clang++-18}
"$CXX" -std=c++20 -O1 -g -Wall -Wextra -Werror "$here/test_leaveplan.cpp" -o "$work/test_leaveplan"

# 176 review: main-boot.cpp cannot be built on a PC, so the glue between the flag files and the header's inputs is pinned by its text. A swapped pair (menufast
# read into the menurelaunch input, say) would turn the instant route into the slow one and back, and no check of the header alone sees it.
main="$here/../../main-boot.cpp"
missing=0
pin() {
	if ! grep -qF -- "$1" "$main"; then
		echo "FAIL  main-boot.cpp no longer has: $1"
		missing=1
	fi
}
pin 'fl.menuexec = orbis_flag("menuexec");'
pin 'fl.menuclose = orbis_flag("menuclose");'
pin 'fl.menufast = orbis_flag("menufast");'
pin 'fl.menurelaunch = orbis_flag("menurelaunch");'
pin 'fl.settle_ms = orbis_leave::SettleMs(has_settle ? settle_text.c_str() : nullptr);'
pin 'in.flag_menuexec = fl.menuexec;'
pin 'in.flag_menuclose = fl.menuclose;'
pin 'in.flag_menufast = fl.menufast;'
pin 'in.flag_menurelaunch = fl.menurelaunch;'
pin 'in.is_tile = s_tile_marker_seen && !s_tile_marker_drive.empty();'
pin 'if (!orbis_leave::WantRelay(want))'
pin 'timing.settle_ms = fl.settle_ms;'
pin 'How how = orbis_leave::Decide(in);'
# fix180: the note names the title the leave starts, and main() takes only a note addressed to it
pin 'orbis_mark_open_shelf(orbis_leave::TargetOf(how, own_tile.c_str()));'
pin 'fprintf(f, "%lld %lld %lld %s\n",'
pin 'const bool open_shelf = orbis_take_open_shelf_mark(&s_leave_mark, true);'
pin 'if (!orbis_leave::NoteIsFor(mark, own.c_str(), orbis_is_tile_start()))'
# fix180: the exec is guarded (relaunch/relaunch_guard.h), the guard is called off when the process stays, and the next start tells it it came back
pin 'if (orbis_leave::WantGuard(want) && OrbisRelaunchBuilt())'
pin 'timing.guard_ms = orbis_leave::kGuardMs;'
pin 'if (!waiting && s_leave_guard_up)'
pin 'const std::string back = OrbisRelaunchConfirmCameBack();'
if grep -qF 'orbis_mark_open_shelf();' "$main"; then
	echo "FAIL  main-boot.cpp writes an open-shelf note without a title again"
	missing=1
fi
[ "$missing" = 0 ] || exit 1
echo "leaveplan glue: main-boot.cpp feeds the header as the test assumes"
"$work/test_leaveplan"
