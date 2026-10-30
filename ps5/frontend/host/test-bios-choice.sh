#!/usr/bin/env bash
# bioscheck (2026-10-29, AI-assisted; owner: a clear BIOS check before any start, and OpenPS2BIOS shipped for players without a BIOS): the BIOS
# question on the shelf in fe_host (SwiftShader stands in for the GPU). With no BIOS a pick doesn't start: the question offers the open-source
# BIOS (OpenPS2BIOS, experimental) and the player's own; picking the open one writes gs.ini PS5SX2/OpenBios=true and starts the pick; Circle
# closes the question; a start from outside the shelf (Play from disc, a finished copy) is not taken and asks once; a tile's or disc's start
# refused before the shelf came up asks at once and starts when the open BIOS is picked; with the open BIOS chosen nothing asks and the shelf
# names it; the sheet's switch turns it off again. Without the open BIOS in the build only "your own" is offered. Pictures go to $FE_SHOTS.
#
#   ps5/frontend/host/test-bios-choice.sh
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
"$here/build-host.sh" >/dev/null
work=$(mktemp -d "${TMPDIR:-/tmp}/bios-choice.XXXXXX")
trap 'rm -rf "$work"' EXIT
icd=${VK_ICD_FILENAMES:-$(ls /opt/pw-browsers/*/chrome-linux/vk_swiftshader_icd.json 2>/dev/null | head -1 || true)}
[ -n "$icd" ] && export VK_ICD_FILENAMES=$icd # else the machine's own Vulkan (lavapipe)
shots=${FE_SHOTS:-$work/shots}
failed=0
check() { # check <what> <command...>
  local what=$1
  shift
  if "$@"; then echo "ok: $what"; else echo "FAILED: $what"; failed=1; fi
}
run() { # run <name> <data folder> <fe_host arguments and steps...>
  local name=$1 data=$2
  shift 2
  if out=$("$here/fe_host" --data "$data" --out "$shots" --size 960x540 "$@" 2>&1) && ! grep -q "expect FAILED" <<<"$out"; then
    echo "ok: $name"
  else
    echo "FAILED: $name"
    grep "expect\|\[host\]\|BIOS\|bios" <<<"$out" | grep -v "^\[host\] device" || true
    failed=1
  fi
}
ini() { grep -s "^$2=" "$1/gs.ini" | head -1 | cut -d= -f2-; }
nobios=(--no-bios "In /data/PCSX2/bios: the folder is empty.")

# gamedb (2026-10-30): resources/GameIndex.yaml missing: the shelf names it under the BIOS line (alone, and with the BIOS line up); not without.
gdb=(--no-gamedb /data/PCSX2/resources/GameIndex.yaml)
run gamedb-note "$work/gamedb-note" "${gdb[@]}" "wait 1" "expect gamedbnote=1" "expect biosmissing=0" "shot gamedb-note"
run gamedb-note-nobios "$work/gamedb-note-nobios" "${gdb[@]}" "${nobios[@]}" "wait 1" "expect gamedbnote=1" "expect biosmissing=1" "shot gamedb-note-nobios"
run gamedb-none "$work/gamedb-none" "wait 1" "expect gamedbnote=0"

# Without the open BIOS in the build: a pick asks, only "your own" is offered; Cross on it says where, and nothing starts.
d="$work/own-only"
run own-only "$d" "${nobios[@]}" "game 0" "wait 1" "expect biosmissing=1" "shot bios-line" "press cross" "wait 0.4" "expect biosask=1" \
  "expect biosrow=0" "expect done=0" "shot bios-question-own-only" "press right" "wait 0.2" "expect biosrow=0" "press cross" "wait 0.4" \
  "expect biosask=0" "expect done=0" "expect biosmissing=1" "shot bios-own-said"
check "nothing chose the open BIOS" test -z "$(ini "$d" PS5SX2/OpenBios)"

# With it: the question offers the open BIOS first; Right/Left move; Circle closes it and the game stays.
d="$work/circle"
run circle "$d" "${nobios[@]}" --open-bios "game 1" "wait 1" "shot bios-line-open" "press cross" "wait 0.4" "expect biosask=1" "expect biosrow=0" \
  "shot bios-question" "press right" "wait 0.2" "expect biosrow=1" "shot bios-question-own" "press left" "wait 0.2" "expect biosrow=0" \
  "press circle" "wait 0.4" "expect biosask=0" "expect done=0" "expect openchosen=0"

# Picking the open BIOS remembers it and starts the pick.
d="$work/open"
run open "$d" "${nobios[@]}" --open-bios "game 2" "wait 1" "press cross" "wait 0.4" "expect biosask=1" "press cross" "wait 1" \
  "expect biosask=0" "expect openchosen=1" "expect biosmissing=0" "expect done=1" "expect selected=2" "expect systemmenu=0"
check "gs.ini says PS5SX2/OpenBios=true" test "$(ini "$d" PS5SX2/OpenBios)" = "true"

# "Your own" on the two-card question: nothing written, nothing started.
d="$work/own"
run own "$d" "${nobios[@]}" --open-bios "game 0" "wait 1" "press cross" "wait 0.4" "press right" "wait 0.2" "press cross" "wait 0.4" \
  "expect biosask=0" "expect done=0" "expect openchosen=0"
check "the player's own: gs.ini untouched" test -z "$(ini "$d" PS5SX2/OpenBios)"

# A start from outside the shelf (Play from disc): not taken, the question comes up once for it; picking the open BIOS lets it go.
d="$work/outside"
run outside "$d" "${nobios[@]}" --open-bios "game 0" "wait 1" "outside /dev/cd0" "expect outsideok=0" "wait 0.4" "expect biosask=1" \
  "press circle" "wait 0.4" "expect biosask=0" "outside /dev/cd0" "wait 0.4" "expect outsideok=0" "expect biosask=0" \
  "outside /data/PCSX2/games/Copy (SLUS-00000).iso" "wait 0.4" "expect biosask=1" "press cross" "wait 0.6" "expect approved=1" "expect done=1"

# A tile's start refused before the shelf came up: the question is up at once, and the open BIOS starts that start.
d="$work/pending"
run pending "$d" "${nobios[@]}" --open-bios --bios-pending /dev/cd0 "game 0" "wait 0.6" "expect biosask=1" "shot bios-question-pending" \
  "press cross" "wait 0.6" "expect approved=1" "expect done=1" "expect openchosen=1"
# The same without the open BIOS: only "your own"; Circle leaves the shelf up.
d="$work/pending-own"
run pending-own "$d" "${nobios[@]}" --bios-pending /dev/cd0 "game 0" "wait 0.6" "expect biosask=1" "press circle" "wait 0.4" \
  "expect biosask=0" "expect done=0" "expect approved=0"

# Chosen before: nothing asks, an outside start is taken, the shelf names the open BIOS.
d="$work/chosen"
mkdir -p "$d"
printf 'PS5SX2/OpenBios=true\n' > "$d/gs.ini"
run chosen "$d" "${nobios[@]}" --open-bios "game 0" "wait 1" "expect biosmissing=0" "shot bios-open-in-use" "outside /dev/cd0" \
  "expect outsideok=1" "expect biosask=0"
run chosen-pick "$d" "${nobios[@]}" --open-bios "game 0" "wait 1" "press cross" "wait 1" "expect biosask=0" "expect done=1"

# Switched back on the sheet for all games (Folders: the row after Scan subfolders, 7 down from the top): Cross turns it off; the next pick asks.
d="$work/switch-back"
mkdir -p "$d"
printf 'PS5SX2/OpenBios=true\n' > "$d/gs.ini"
run switch-back "$d" "${nobios[@]}" --open-bios "game 0" "wait 1" "press square" "wait 0.4" "press r1" "wait 0.3" "press up 80" "wait 0.1" \
  "press down 7" "wait 0.2" "shot bios-sheet-row" "press cross" "wait 0.3" "expect openchosen=0" "press circle" "wait 0.5" "expect sheet=0" \
  "expect biosmissing=1" "press cross" "wait 0.4" "expect biosask=1" "expect done=0"

exit $failed
