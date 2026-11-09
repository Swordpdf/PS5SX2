#!/usr/bin/env bash
# Real shelf input, filtering and restart checks with synthetic host games. (AI-assisted)
# FE_HOST_BIN can select an already built host harness; FE_SHOTS keeps screenshots.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if [[ -z ${FE_HOST_BIN:-} ]]; then "$here/build-host.sh" >/dev/null; fi
host=${FE_HOST_BIN:-$here/fe_host}
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
data="$work/data"
shots=${FE_SHOTS:-$work/shots}
mkdir -p "$data"
run() {
  local name=$1
  shift
  if ! "$host" --data "$data" --out "$shots" --size 960x540 "$@" > "$work/$name.log" 2>&1; then
    cat "$work/$name.log"
    exit 1
  fi
  if grep -q 'expect FAILED' "$work/$name.log"; then cat "$work/$name.log"; exit 1; fi
  echo "PASS: $name"
}
ini() { grep -s "^$2=" "$1" | tail -1 | cut -d= -f2-; }
gow="$data/settings/God of War (USA).ini"
gt="$data/settings/Gran Turismo 4 (USA).ini"

# The first two game-specific rows after Recommended are Hide and Favorite.
run favorite-first "game 0" "wait 1" "expect shelf=5" "press square" "wait 0.4" "press down 2" "shot favorite-row" \
  "press cross" "press circle" "wait 0.5" "expect shelf=5" "expect selected=0" "shot favorite-marked"
[[ $(ini "$gow" PS5SX2/Favorite) == true ]]
run favorite-second "game 2" "wait 1" "press square" "wait 0.4" "press down 2" "press cross" "press circle" "wait 0.5" \
  "expect selected=2" "press square" "wait 0.4" "press r1" "wait 0.4" "press down 9" "wait 0.3" "shot favorite-view-row" \
  "press cross" "press circle" "wait 0.5" "expect shelf=2" "expect selected=2" "shot favorites-shelf" \
  "remote god" "wait 0.4" "expect shelf=1" "expect selected=0" "remote" "wait 0.4" "expect shelf=2"
[[ $(ini "$gt" PS5SX2/Favorite) == true ]]
[[ $(ini "$data/gs.ini" PS5SX2/ShelfView) == favorites ]]

# Restart, retain original identities, and remove the final two favorites without losing access to settings.
run remove-last-favorites "game 0" "wait 1" "expect shelf=2" "expect selected=0" \
  "press square" "wait 0.4" "press down 2" "press cross" "press circle" "wait 0.5" "expect shelf=1" "expect selected=2" \
  "press square" "wait 0.4" "press down 2" "press cross" "press circle" "wait 0.5" "expect shelf=0" "shot favorites-empty" \
  "press square" "wait 0.4" "expect sheet=1" "press down 9" "press cross" "press circle" "wait 0.5" \
  "expect shelf=5" "shot favorites-restored"
[[ $(ini "$gow" PS5SX2/Favorite) == false ]]
[[ $(ini "$gt" PS5SX2/Favorite) == false ]]
[[ $(ini "$data/gs.ini" PS5SX2/ShelfView) == all ]]

# Hidden favorites stay hidden until Show hidden games is selected, even with every other game filtered out.
run hidden-favorite "game 0" "wait 1" "press square" "wait 0.4" "press down 2" "press cross" "press up" "press cross" \
  "press circle" "wait 0.5" "expect shelf=4" "expect selected=1" \
  "press square" "wait 0.4" "press r1" "wait 0.4" "press down 9" "press cross" "press circle" "wait 0.5" "expect shelf=0" \
  "press square" "wait 0.4" "press down 8" "press cross" "press circle" "wait 0.5" "expect shelf=1" "expect selected=0" \
  "shot favorite-hidden-shown" "remote missing" "wait 0.4" "expect shelf=0" "remote" "wait 0.4" "expect shelf=1"

# Physical-disc actions remain reachable; the filter only affects library images.
run drive-with-favorites "game 0" "wait 1" "expect shelf=1" "drive SLUS-99999 Host Disc" "wait 0.5" \
  "expect driveentry=1" "expect shelf=2" "press left" "expect selected=-2" "drive off" "wait 0.5" "expect shelf=1"
run delayed-scan "scan 0/5" "wait 0.4" "expect shelf=0" "scan done" "wait 0.5" "expect shelf=1" "expect selected=0"
run empty-search-view-change "game 0" "wait 1" "remote missing" "wait 0.3" "expect shelf=0" \
  "press square" "wait 0.4" "press down 9" "press cross" "press circle" "wait 0.4" "expect shelf=0" \
  "expect searchlen=7" "remote" "wait 0.3" "expect shelf=5"
echo 'PASS: Favorites shelf workflows'
