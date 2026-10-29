#!/usr/bin/env bash
# 180 (AI-assisted): the sheet for all games' PS5SX2 group in fe_host (fe_update.h), over a stand-in GitHub and installer (--updates):
#   - System update: Cross looks; a newer release is offered and Cross twice starts the installer, then the shelf asks to close (3 s later);
#     a single Cross does not; the same release, an older one, no network, an HTTP error and an installer that can't start each say so;
#   - Session logs: Cross and Right switch the installer's no-log-upload file.
# The group sits at the bottom of the sheet for all games, above the reset row: from the bottom, up once is Session logs, twice System update.
#
#   ps5/frontend/host/test-update-rows.sh        (VK_ICD_FILENAMES: a Vulkan driver, e.g. lavapipe's lvp_icd.json; FE_SHOTS=<dir> keeps pictures)
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
"$here/build-host.sh" >/dev/null
work=$(mktemp -d "${TMPDIR:-/tmp}/update-rows.XXXXXX")
trap 'rm -rf "$work"' EXIT
export VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-$(ls /usr/share/vulkan/icd.d/lvp_icd*.json /opt/pw-browsers/*/chrome-linux/vk_swiftshader_icd.json 2>/dev/null | head -1)}
shots=${FE_SHOTS:-$work/shots}
failed=0
run() {
  local name=$1
  shift
  if out=$("$here/fe_host" --data "$work/$name" --out "$shots" --size 960x540 --build-tag vk-285-180 "$@" 2>&1) &&
    ! grep -q "expect FAILED" <<<"$out"; then
    echo "ok: $name"
  else
    echo "FAILED: $name"
    grep "expect\|\[host\]\|\[update\]" <<<"$out" | grep -v "^\[host\] device" || true
    failed=1
  fi
}
to_update=("game 2" "wait 1" "press square" "wait 0.4" "press r1" "wait 0.3" "press down 70" "press up" "press up" "wait 0.2")
to_logs=("game 2" "wait 1" "press square" "wait 0.4" "press r1" "wait 0.3" "press down 70" "press up" "wait 0.2")
# UpdateStatus::State: 0 idle, 2 current, 3 newer, 4 available, 5 failed, 8 started, 9 the installer didn't start
run available --updates vk-285-181 "${to_update[@]}" "expect updstate=0" "shot update-idle" "press cross" "wait 1" "expect updstate=4" \
  "shot update-available" "press cross" "wait 0.3" "expect updstarted=0" "shot update-armed" "press cross" "wait 0.3" "expect updstarted=1" \
  "expect updstate=8" "expect updquit=0" "shot update-started" "wait 3.5" "expect updquit=1"
run not-armed-twice --updates vk-285-181 "${to_update[@]}" "press cross" "wait 1" "press cross" "wait 4.5" "press cross" "wait 0.3" \
  "expect updstarted=0" "expect updstate=4" "press cross" "wait 0.3" "expect updstarted=1"
run current --updates vk-285-180 "${to_update[@]}" "press cross" "wait 1" "expect updstate=2" "press cross" "wait 1" "press cross" "wait 0.3" \
  "expect updstarted=0" "shot update-current"
run newer --updates vk-285-171 "${to_update[@]}" "press cross" "wait 1" "expect updstate=3" "shot update-newer"
run no-network --updates fail "${to_update[@]}" "press cross" "wait 1" "expect updstate=5" "shot update-failed"
run http-error --updates error "${to_update[@]}" "press cross" "wait 1" "expect updstate=5"
run no-loader --updates vk-285-181 --updates-start 3 "${to_update[@]}" "press cross" "wait 1" "press cross" "wait 0.2" "press cross" "wait 0.3" \
  "expect updstate=9" "expect updstarted=0" "shot update-noloader" "wait 3.5" "expect updquit=0"
run logs --updates vk-285-181 "${to_logs[@]}" "expect logupload=1" "shot logs-on" "press cross" "wait 0.3" "expect logupload=0" "shot logs-off" \
  "press right" "wait 0.3" "expect logupload=1" "press left" "wait 0.3" "expect logupload=0"
[[ -f $work/logs/installer/no-log-upload ]] || { echo "FAILED: logs (no-log-upload not written)"; failed=1; }
# Owner, 2026-10-29: session logs on by default (logupload=1 above with no switch file) and no notice about them on the screen.
if grep -q "log-notice-shown\|NotifyLogsNotice\|logs_notice" "$here/../fe_ps5.cpp" "$here/../fe_i18n.cpp" "$here/../fe_i18n.h"; then
  echo "FAILED: a notice about the session logs is back"
  failed=1
else
  echo "ok: no session-log notice"
fi
# Without --updates there is no group: the row above the reset row is still the PS2 system menu (test-shelf-controls.sh relies on it).
run no-group "${to_logs[@]}" "press cross" "wait 0.3" "press cross" "wait 1" "expect systemmenu=1" "expect updstate=-1"
if [[ $failed != 0 ]]; then
  echo "update rows: FAILED"
  exit 1
fi
echo "update rows: all passed"
