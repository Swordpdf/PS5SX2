#!/usr/bin/env bash
# pr9n (AI-assisted), PR #9 review item 11: the shelf's controls in fe_host (SwiftShader stands in for the GPU).
#   - L1 + Square opens the RetroAchievements account in either order: L1 first used to jump 5 games before the chord
#     was seen (the selection now goes back), Square first used to open the settings sheet and L1 then switched it to
#     This game (the sheet now closes and the account opens);
#   - L1 alone still jumps 5 games;
#   - the sheet's tabs stop at either end (L2 from Settings went round to Achievements, which starts network traffic).
#
#   ps5/frontend/host/test-shelf-controls.sh
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
"$here/build-host.sh" >/dev/null
work=$(mktemp -d "${TMPDIR:-/tmp}/shelf-controls.XXXXXX")
trap 'rm -rf "$work"' EXIT
export VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-$(ls /opt/pw-browsers/*/chrome-linux/vk_swiftshader_icd.json 2>/dev/null | head -1)}
failed=0
run() {
  local name=$1
  shift
  if out=$("$here/fe_host" --data "$work/$name" --out "$work/shots" --size 960x540 --achievements-preview "$@" 2>&1) &&
    ! grep -q "expect FAILED" <<<"$out"; then
    echo "ok: $name"
  else
    echo "FAILED: $name"
    grep "expect\|\[host\]" <<<"$out" | grep -v "^\[host\] device" || true
    failed=1
  fi
}
# The 5 host games; "game 4" selects the last one.
run l1-then-square "game 4" "wait 1" "down l1" "wait 0.15" "expect selected=0" "down square" "wait 0.3" "expect account=1" \
  "expect selected=4" "up l1+square" "wait 0.3" "press circle" "wait 0.5" "expect account=0" "expect selected=4"
run square-then-l1 "game 4" "wait 1" "down square" "wait 0.15" "expect sheet=1" "down l1" "wait 0.3" "expect sheet=0" \
  "expect account=1" "expect selected=4" "up l1+square" "wait 0.3"
run both-at-once "game 4" "wait 1" "press l1+square" "wait 0.3" "expect account=1" "expect selected=4"
run l1-alone "game 4" "wait 1" "press l1" "wait 0.6" "expect selected=0" "expect account=0" "expect sheet=0"
run late-square "game 4" "wait 1" "down l1" "wait 0.8" "down square" "wait 0.3" "expect account=1" "expect selected=0" "up l1+square"
# 2026-10-08: the sheet for all games' "PS2 system menu" (the row above "Reset to PCSX2's defaults"): one Cross arms it, a
# second starts it and the shelf closes without a game.
run system-menu "game 2" "wait 1" "press square" "wait 0.4" "press r1" "wait 0.3" "press down 70" "press up" "wait 0.2" \
  "press cross" "wait 0.3" "expect systemmenu=0" "expect sheet=1" "press cross" "wait 1" "expect systemmenu=1" "expect done=1" \
  "expect sheet=0"
run system-menu-not-armed "game 2" "wait 1" "press square" "wait 0.4" "press r1" "wait 0.3" "press down 70" "press up" "wait 0.2" \
  "press cross" "wait 4.5" "press cross" "wait 1" "expect systemmenu=0" "expect done=0" "expect sheet=1"
run tabs-stop "game 4" "wait 1" "press square" "wait 0.5" "expect tab=0" "press l2" "wait 0.3" "expect tab=0" "press r2" "wait 0.3" \
  "expect tab=1" "press r2" "wait 0.3" "expect tab=2" "press r2" "wait 0.3" "expect tab=2" "press l2" "wait 0.3" "expect tab=1"
if [[ $failed != 0 ]]; then
  echo "shelf controls: FAILED"
  exit 1
fi
echo "shelf controls: all passed"
