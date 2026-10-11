#!/usr/bin/env bash
# vk-285-135 (AI-assisted; swordpdf: "i also want to pick a folder though the browser in the shelf"): the sheet for all games'
# Folders rows in fe_host (SwiftShader stands in for the GPU): the picker goes into a "drive" and a folder in it, says how
# many games are there, and "Use this folder" adds it to PS5SX2/GameFolders in gs.ini; the folder added is at the top of the
# places and Triangle removes it; the BIOS folder is picked and Triangle on its row sets it back; an NFS share is typed on
# the (stand-in) PS5 keyboard, listed, and removed. Pictures of the picker go to $FE_SHOTS when it is set.
#
#   ps5/frontend/host/test-folder-picker.sh
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
"$here/build-host.sh" >/dev/null
work=$(mktemp -d "${TMPDIR:-/tmp}/folder-picker.XXXXXX")
trap 'rm -rf "$work"' EXIT
export VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-$(ls /opt/pw-browsers/*/chrome-linux/vk_swiftshader_icd.json 2>/dev/null | head -1)}
shots=${FE_SHOTS:-$work/shots}
data="$work/data"
mkdir -p "$data/usb0/PS2/Sub" "$data/usb0/Other" "$data/usb0/.hidden"
: > "$data/usb0/PS2/Game A (USA).iso"
: > "$data/usb0/PS2/Sub/Game B (Europe).chd"
: > "$data/usb0/PS2/notes.txt"
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
    grep "expect\|\[host\]\|\[options\]" <<<"$out" | grep -v "^\[host\] device" || true
    failed=1
  fi
}
ini() { grep -s "^$1=" "$data/gs.ini" | head -1 | cut -d= -f2-; }
# The sheet for all games (Square, then R1), its first rows (vk-285-135b: Folders at the top): Recommended for all games, then
# Game folders, BIOS folder, NFS shares. Each step below goes down from the top.
open_all="game 0;wait 1;press square;wait 0.4;press r1;wait 0.3;press up 80;wait 0.1"
steps() { tr ';' '\n' <<<"$1" | sed '/^$/d'; }
script="$work/script.txt"

# A game folder: USB drive 1 > PS2 (2 games, one in its folder Sub) > Use this folder.
steps "$open_all;press down 1;wait 0.2;press cross;wait 0.3;expect picker=1;shot picker-places;press down;press cross;wait 0.2;shot picker-usb;press down 2;press cross;wait 0.2;shot picker-ps2;press cross;wait 0.3;expect picker=0;expect sheet=1;shot picker-added" > "$script"
run game-folder --script "$script"
check "the folder is in PS5SX2/GameFolders" test "$(ini PS5SX2/GameFolders)" = "$data/usb0/PS2"

# Again: the added folder heads the places; Triangle removes it.
steps "$open_all;press down 1;wait 0.2;press cross;wait 0.3;expect picker=1;press triangle;wait 0.3;expect picker=1;press circle;wait 0.3;expect picker=0;expect sheet=1" > "$script"
run game-folder-removed --script "$script"
check "the game folder is gone from gs.ini" test -z "$(ini PS5SX2/GameFolders)"

# The BIOS folder: USB drive 1 > Use this folder; then Triangle on the row sets it back.
steps "$open_all;press down 2;wait 0.2;press cross;wait 0.3;press down;press cross;wait 0.2;press cross;wait 0.3;expect picker=0" > "$script"
run bios-folder --script "$script"
check "PS5SX2/BiosFolder is the drive" test "$(ini PS5SX2/BiosFolder)" = "$data/usb0"
steps "$open_all;press down 2;wait 0.2;press triangle;wait 0.3;expect picker=0" > "$script"
run bios-folder-reset --script "$script"
check "PS5SX2/BiosFolder is unset again" test -z "$(ini PS5SX2/BiosFolder)"

# An NFS share typed on the keyboard, listed, then removed.
steps "$open_all;press down 3;wait 0.2;press cross;wait 0.3;expect picker=1;press cross;wait 0.4;shot picker-nfs;expect picker=1" > "$script"
run nfs-share --ime-text "nfs://192.168.1.10/volume1/PS2" --script "$script"
check "the share is in PS5SX2/NfsShares" test "$(ini PS5SX2/NfsShares)" = "nfs://192.168.1.10/volume1/PS2"
# vk-285-138: Cross on a listed share changes it on the keyboard (it takes the old one's place); not nfs://: it stays.
steps "$open_all;press down 3;wait 0.2;press cross;wait 0.3;press down;press cross;wait 0.4;expect picker=1" > "$script"
run nfs-share-edited --ime-text "nfs://192.168.1.20/volume1/Games" --script "$script"
check "the share was changed in place" test "$(ini PS5SX2/NfsShares)" = "nfs://192.168.1.20/volume1/Games"
run nfs-share-edit-refused --ime-text "smb://pc/games" --script "$script"
check "an smb:// edit leaves the share" test "$(ini PS5SX2/NfsShares)" = "nfs://192.168.1.20/volume1/Games"
steps "$open_all;press down 3;wait 0.2;press cross;wait 0.3;press down;press triangle;wait 0.3;press circle;wait 0.3;expect picker=0" > "$script"
run nfs-share-removed --script "$script"
check "the share is gone from gs.ini" test -z "$(ini PS5SX2/NfsShares)"
# vk-285-139: no PS5 keyboard (no --ime): the panel's own keyboard. Up to "1", Cross three times, Options: "nfs://111".
steps "$open_all;press down 3;wait 0.2;press cross;wait 0.3;press cross;wait 0.3;press up;press cross 3;wait 0.2;shot picker-osk;press options;wait 0.3;expect picker=1" > "$script"
run nfs-share-osk --script "$script"
check "the panel's keyboard typed the share" test "$(ini PS5SX2/NfsShares)" = "nfs://111"
# Changing it there: Square deletes a character, Options keeps it.
steps "$open_all;press down 3;wait 0.2;press cross;wait 0.3;press down;press cross;wait 0.3;press square;press options;wait 0.3" > "$script"
run nfs-share-osk-edit --script "$script"
check "the panel's keyboard changed the share" test "$(ini PS5SX2/NfsShares)" = "nfs://11"
# Circle cancels: it stays.
steps "$open_all;press down 3;wait 0.2;press cross;wait 0.3;press down;press cross;wait 0.3;press square;press circle;wait 0.3;expect picker=1" > "$script"
run nfs-share-osk-cancel --script "$script"
check "Circle leaves the share" test "$(ini PS5SX2/NfsShares)" = "nfs://11"
steps "$open_all;press down 3;wait 0.2;press cross;wait 0.3;press down;press triangle;wait 0.3" > "$script"
run nfs-share-osk-removed --script "$script"
# Not an nfs:// address: nothing added.
steps "$open_all;press down 3;wait 0.2;press cross;wait 0.3;press cross;wait 0.4" > "$script"
run nfs-share-refused --ime-text "smb://pc/games" --script "$script"
check "an smb:// address isn't added" test -z "$(ini PS5SX2/NfsShares)"

if [[ $failed != 0 ]]; then
  echo "folder picker: FAILED"
  exit 1
fi
echo "folder picker: all passed"
