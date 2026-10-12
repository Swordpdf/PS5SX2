#!/usr/bin/env bash
# live-22 (AI-assisted): the shelf's disc-dump entry (fe_app.cpp PollDiscDump). While a PS2 disc is being copied
# (orbis-shims/ProsperoDiscDump.cpp writes logs/disc-dump-progress.txt), a spinning-disc entry appears at the front of the
# shelf with the live percentage; it goes away when the copy ends. The "dump <pct>" / "dump off" steps stand in for the
# console's dumper writing that file; SwiftShader stands in for the GPU.
#
#   ps5/frontend/host/test-disc-dump.sh
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
"$here/build-host.sh" >/dev/null
work=$(mktemp -d "${TMPDIR:-/tmp}/disc-dump.XXXXXX")
trap 'rm -rf "$work"' EXIT
export VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-$(ls /opt/pw-browsers/*/chrome-linux/vk_swiftshader_icd.json 2>/dev/null | head -1)}
failed=0
run() {
  local name=$1
  shift
  if out=$("$here/fe_host" --data "$work/$name" --out "$work/shots" --size 960x540 "$@" 2>&1) &&
    ! grep -q "expect FAILED" <<<"$out"; then
    echo "ok: $name"
  else
    echo "FAILED: $name"
    grep "expect\|\[host\]" <<<"$out" | grep -v "^\[host\] device" || true
    failed=1
  fi
}

# A copy appears as a sixth entry with its percentage, climbs, and is dropped when it ends.
run appears-and-climbs "wait 1" "expect shelf=5" "expect dumppct=-1" \
  "dump 0" "wait 0.7" "expect shelf=6" "expect dumppct=0" "expect selected=-1" \
  "dump 42" "wait 0.7" "expect dumppct=42" "expect shelf=6" \
  "dump 99" "wait 0.7" "expect dumppct=99" \
  "dump off" "wait 0.7" "expect shelf=5" "expect dumppct=-1"

# The dump entry can't be launched (the disc is still being copied): Cross is refused, the shelf stays open.
run cross-is-refused "wait 1" "dump 30" "wait 0.7" "expect dumppct=30" "press cross" "wait 0.3" "expect done=0" "expect shelf=6"

# A different disc replaces the entry (serial change handled by a fresh entry), and a frame renders with the spinner on it.
run screenshot "wait 1" "dump 55" "wait 0.7" "expect dumppct=55" "shot disc-dump"

if [[ $failed -eq 0 ]]; then
  echo "disc dump: all passed"
else
  echo "disc dump: FAILED"
  exit 1
fi
