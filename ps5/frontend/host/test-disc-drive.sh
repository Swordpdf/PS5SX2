#!/usr/bin/env bash
# The disc drive's box on the shelf (fe_host --drive, its "drive" steps standing in for fe_ps5.cpp's DriveWatch): off the
# shelf while the drive is empty, on it and selected with a spinning disc while a disc is read (it won't start then), the
# game's title, serial and region once known (it starts), off again when the disc comes out or isn't a PS2 game, and a disc
# that goes in while the options sheet is open doesn't take the selection. Pictures go to $FE_SHOTS when it is set.
#
#   ps5/frontend/host/test-disc-drive.sh
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
"$here/build-host.sh" >/dev/null
work=$(mktemp -d "${TMPDIR:-/tmp}/disc-drive.XXXXXX")
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
  if out=$("$here/fe_host" --drive --data "$data" --out "$shots" --size 960x540 "$@" 2>&1) && ! grep -q "expect FAILED" <<<"$out"; then
    echo "ok: $name"
  else
    echo "FAILED: $name"
    grep "expect\|\[host\]\|\[frontend\]" <<<"$out" | grep -v "^\[host\] device\|cover for" || true
    failed=1
  fi
  last_out=$out
}
kh="drive ready SLUS-20370|Kingdom Hearts||2900"

# 1. The drive is empty: five boxes, the drive's (index 0) isn't one of them. A disc goes in: the drive's box joins the
# shelf, selected, reading; Cross doesn't start it. Then it's Kingdom Hearts, and Cross starts it.
run insert "game 1" "wait 1" "expect shelf=5" "expect selected=1" "drive reading" "wait 0.6" "expect shelf=6" \
  "expect selected=0" "shot drive-reading" "press cross" "wait 0.5" "expect done=0" "$kh" "wait 0.6" "expect shelf=6" \
  "expect selected=0" "shot drive-game" "press cross" "wait 1" "expect done=1" "expect selected=0"
check "the shelf logged the reading" grep -q "disc drive /dev/cd1: reading the disc" <<<"$last_out"
check "and the game" grep -q "disc drive /dev/cd1: Kingdom Hearts (SLUS-20370)" <<<"$last_out"

# 2. The disc comes out while its box is selected: off the shelf, the next game selected.
run eject "game 1" "wait 1" "$kh" "wait 0.6" "expect selected=0" "drive empty" "wait 0.6" "expect shelf=5" "expect selected=1"

# 3. A disc that isn't a PS2 game: the box never joins the shelf (it's left after the reading).
run other "game 2" "wait 1" "drive reading" "wait 0.4" "expect shelf=6" "drive other" "wait 0.6" "expect shelf=5" \
  "expect selected=2"

# 4. A disc goes in while the options sheet is open over another game: the box joins the shelf, the selection stays.
run sheet "game 2" "wait 1" "press square" "wait 0.5" "expect sheet=1" "$kh" "wait 0.6" "expect shelf=6" "expect selected=2" \
  "expect sheet=1"

if ((failed)); then
  echo "test-disc-drive: FAILED"
  exit 1
fi
echo "test-disc-drive: all passed"
