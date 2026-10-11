#!/usr/bin/env bash
# vk-285-137 (AI-assisted; swordpdf: "add an option to hide games from the shelf"): fe_host (SwiftShader stands in for the GPU)
# hides a game from its sheet (Shelf > Hide from the shelf), starts again without it, shows hidden games from the sheet for all
# games (dimmed), takes the game's hide off again, and hides the last game (the one before it is selected). Pictures go to
# $FE_SHOTS when it is set.
#
#   ps5/frontend/host/test-hide-games.sh
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
"$here/build-host.sh" >/dev/null
work=$(mktemp -d "${TMPDIR:-/tmp}/hide-games.XXXXXX")
trap 'rm -rf "$work"' EXIT
export VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-$(ls /opt/pw-browsers/*/chrome-linux/vk_swiftshader_icd.json 2>/dev/null | head -1)}
shots=${FE_SHOTS:-$work/shots}
data="$work/data"
mkdir -p "$data"
failed=0
check() { # check <what> <command...>
  local what=$1
  shift
  if "$@"; then echo "ok: $what"; else echo "FAILED: $what"; failed=1; fi
}
run() {
  local name=$1
  shift
  if out=$("$here/fe_host" --data "$data" --out "$shots" --size 960x540 "$@" 2>&1) && ! grep -q "expect FAILED" <<<"$out"; then
    echo "ok: $name"
  else
    echo "FAILED: $name"
    grep "expect\|\[host\]\|\[frontend\]\|\[options\]" <<<"$out" | grep -v "^\[host\] device\|cover for" || true
    failed=1
  fi
  last_out=$out
}
ini() { grep -s "^$2=" "$1" | tail -1 | cut -d= -f2-; }
gow="$data/settings/God of War (USA).ini"
steps() { tr ';' '\n' <<<"$1" | sed '/^$/d'; }
script="$work/script.txt"
# A game's sheet: Recommended settings, then Shelf > Hide from the shelf. The sheet for all games: Recommended for all games,
# the three Folders rows, then Shelf > Show hidden games.
hide_row="press square;wait 0.4;press up 80;wait 0.1;press down 1;wait 0.2"
show_row="press square;wait 0.4;press r1;wait 0.3;press up 80;wait 0.1;press down 4;wait 0.2"

# 1. God of War (game 0) hidden from its sheet: off the shelf when the sheet closes, the next game selected, a note.
steps "game 0;wait 1;expect shelf=5;expect selected=0;$hide_row;shot hide-row;press cross;wait 0.3;press circle;wait 0.5;expect sheet=0;expect shelf=4;expect selected=1;shot hidden-note" > "$script"
run hide --script "$script"
check "God of War's file has PS5SX2/HideGame=true" test "$(ini "$gow" PS5SX2/HideGame)" = "true"
check "the shelf logged it" grep -q "hidden from the shelf: God of War (USA).iso" <<<"$last_out"

# 2. A new start: not on the shelf, even as the last game (the next one is selected); Show hidden games puts it back, dimmed.
steps "game 0;wait 1;expect shelf=4;expect selected=1;$show_row;press cross;wait 0.3;press circle;wait 0.5;expect shelf=5;expect selected=1;press left;wait 0.6;expect selected=0;shot hidden-shown" > "$script"
run show-hidden --script "$script"
check "gs.ini has PS5SX2/ShowHiddenGames=true" test "$(ini "$data/gs.ini" PS5SX2/ShowHiddenGames)" = "true"
check "the start-up line counts it" grep -q "1 of 5 games hidden from the shelf$" <<<"$last_out"
check "and the shelf says when they're shown" grep -q "hidden games shown, dimmed (1; the shelf had 4, now 5)" <<<"$last_out"

# 3. Its sheet takes the hide off (it stays), and Show hidden games goes off with nothing hidden: still five.
steps "game 0;wait 1;expect shelf=5;expect selected=0;$hide_row;press cross;wait 0.3;press circle;wait 0.5;expect shelf=5;$show_row;press cross;wait 0.3;press circle;wait 0.5;expect shelf=5;expect selected=0" > "$script"
run unhide --script "$script"
check "the start-up line says hidden games are shown" grep -q "1 of 5 games hidden from the shelf (shown, dimmed: Show hidden games is on)" <<<"$last_out"
check "God of War's hide is off" test "$(ini "$gow" PS5SX2/HideGame)" = "false"
check "Show hidden games is off" test "$(ini "$data/gs.ini" PS5SX2/ShowHiddenGames)" = "false"

# 4. The last game hidden: the one before it is selected.
steps "game 4;wait 1;expect selected=4;$hide_row;press cross;wait 0.3;press circle;wait 0.5;expect shelf=4;expect selected=3" > "$script"
run hide-last --script "$script"

if [[ $failed != 0 ]]; then
  echo "hide games: FAILED"
  exit 1
fi
echo "hide games: all passed"
