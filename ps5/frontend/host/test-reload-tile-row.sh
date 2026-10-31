#!/usr/bin/env bash
# b185 (AI-assisted; the owner: "allow people to actually delete the reload disc tile, and it won't come back"): the sheet for all games' "Reload PS2
# Disc tile" switch in fe_host (--reload-tile): Cross, Right and Left switch it, and it is the helper's flag file flags/disctile_noreload (made = Off).
# A flag already there (the person deleted the tile on the home screen: helper 171l makes it) shows Off. Without --reload-tile there is no row.
# The row sits at the bottom of the sheet for all games, above the reset row (no updater in these runs): from the bottom, up once is the switch.
#
#   ps5/frontend/host/test-reload-tile-row.sh        (VK_ICD_FILENAMES: a Vulkan driver, e.g. lavapipe's lvp_icd.json; FE_SHOTS=<dir> keeps pictures)
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
"$here/build-host.sh" >/dev/null
work=$(mktemp -d "${TMPDIR:-/tmp}/reload-tile-row.XXXXXX")
trap 'rm -rf "$work"' EXIT
export VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-$(ls /usr/share/vulkan/icd.d/lvp_icd*.json /opt/pw-browsers/*/chrome-linux/vk_swiftshader_icd.json 2>/dev/null | head -1)}
shots=${FE_SHOTS:-$work/shots}
failed=0
run() {
  local name=$1
  shift
  if out=$("$here/fe_host" --data "$work/$name" --out "$shots" --size 960x540 "$@" 2>&1) && ! grep -q "expect FAILED" <<<"$out"; then
    echo "ok: $name"
  else
    echo "FAILED: $name"
    grep "expect\|\[host\]\|\[options\]" <<<"$out" | grep -v "^\[host\] device" || true
    failed=1
  fi
}
to_row=("game 2" "wait 1" "press square" "wait 0.4" "press r1" "wait 0.3" "press down 70" "press up" "wait 0.2")
run switch --reload-tile "${to_row[@]}" "expect reloadfocus=1" "expect reloadtile=1" "shot reload-on" "press cross" "wait 0.3" \
  "expect reloadtile=0" "shot reload-off" "press right" "wait 0.3" "expect reloadtile=1" "press left" "wait 0.3" "expect reloadtile=0"
[[ -f $work/switch/flags/disctile_noreload ]] || { echo "FAILED: switch (flags/disctile_noreload not made by Off)"; failed=1; }
mkdir -p "$work/deleted/flags" && : >"$work/deleted/flags/disctile_noreload"
run deleted --reload-tile "${to_row[@]}" "expect reloadfocus=1" "expect reloadtile=0" "press cross" "wait 0.3" "expect reloadtile=1"
[[ ! -e $work/deleted/flags/disctile_noreload ]] || { echo "FAILED: deleted (On did not delete the flag)"; failed=1; }
run no-row "${to_row[@]}" "expect reloadfocus=0" "expect reloadtile=-1"
if [[ $failed != 0 ]]; then
  echo "reload tile row: FAILED"
  exit 1
fi
echo "reload tile row: all passed"
