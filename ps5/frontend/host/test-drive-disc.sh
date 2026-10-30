#!/usr/bin/env bash
# 2.11 (AI-assisted; swordpdf: "since the emulator knows what game is inside the drive, it would be cool to have the disc in the shelf as well, maybe
# always on first place" and "we can also use the circular disc cover for when a game is dumping"): the PS2 disc in the drive is the first entry on
# the shelf (fe_host's "drive" step stands in for the disc watcher's report), picking it asks the watcher about the disc again, and the disc's own art
# (covers/<serial>_disc.png here; LaunchBox on the console) is drawn on the entry of a disc in the drive and on the one of a disc being copied.
# The host games: God of War, Ratchet & Clank, Gran Turismo 4, Castlevania - Lament of Innocence, The Lord of the Rings - The Two Towers; the "dump"
# step's disc is SLUS-00000. Pictures go to $FE_SHOTS when it is set.
#
#   ps5/frontend/host/test-drive-disc.sh     (needs Python's Pillow for the disc picture)
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
"$here/build-host.sh" >/dev/null
work=$(mktemp -d "${TMPDIR:-/tmp}/drive-test.XXXXXX")
trap 'rm -rf "$work"' EXIT
export VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-$(ls /opt/pw-browsers/*/chrome-linux/vk_swiftshader_icd.json 2>/dev/null | head -1)}
shots=${FE_SHOTS:-$work/shots}
failed=0
run() {
  local name=$1
  shift
  if out=$("$here/fe_host" --data "$work/$name" --out "$shots" --size 960x540 "$@" 2>&1) && ! grep -q "expect FAILED" <<<"$out"; then
    echo "ok: $name"
  else
    echo "FAILED: $name"
    grep "expect\|\[host\]\|disc\|drive" <<<"$out" | grep -v "^\[host\] device" || true
    failed=1
  fi
}
# A disc picture: a round label with a ring, a hole, a red mark at the top (seen turning), a transparent surround.
art() {
  mkdir -p "$work/$1/covers"
  python3 - "$work/$1/covers/SLUS-00000_disc.png" <<'PY'
import sys
from PIL import Image, ImageDraw
n = 700
im = Image.new("RGBA", (n, n), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
d.ellipse([0, 0, n - 1, n - 1], fill=(236, 232, 246, 255))
d.ellipse([40, 40, n - 41, n - 41], fill=(40, 70, 150, 255))
d.ellipse([120, 120, n - 121, n - 121], fill=(230, 170, 60, 255))
d.rectangle([n // 2 - 30, 60, n // 2 + 30, 150], fill=(220, 30, 30, 255))
d.ellipse([n // 2 - 90, n // 2 - 90, n // 2 + 90, n // 2 + 90], fill=(236, 232, 246, 255))
d.ellipse([n // 2 - 40, n // 2 - 40, n // 2 + 40, n // 2 + 40], fill=(0, 0, 0, 0))
im.save(sys.argv[1])
PY
}
# The disc is listed first; the picked game stays picked; the disc is picked with Left, Cross asks the watcher again; Square opens the disc's own sheet
# (gamedb, 2026-10-30: it opened the all-games one, and L1 there a sheet writing settings/.ini, which no start reads: #bugs "disc games ignore their
# own settings"); its file is the one a start from the drive reads (fe_discsettings.h), R1 and L1 switch to all games and back.
run listed "game 2" "wait 1" "expect shelf=5" "expect selected=2" "expect driveentry=0" "drive SLUS-20312 Final Fantasy X" "wait 0.8" \
  "expect driveentry=1" "expect shelf=6" "expect selected=2" "press left 3" "wait 1" "expect selected=-2" "shot drive-entry" \
  "press square" "wait 0.8" "expect sheet=1" "expectsheetfile settings/Final Fantasy X (SLUS-20312).ini" "shot drive-sheet" \
  "press r1" "wait 0.5" "expectsheetfile gs.ini" "press l1" "wait 0.5" "expectsheetfile settings/Final Fantasy X (SLUS-20312).ini" \
  "press circle" "wait 0.8" "expect sheet=0" "expect reasks=0" "press cross" "wait 0.3" "expect reasks=1" "shot drive-asked" \
  "press right" "wait 0.8" "expect selected=0" "drive off" "wait 0.8" "expect driveentry=0" "expect shelf=5" "expect selected=0"
# gamedb (2026-10-30): a settings file an earlier build named (settings/<serial>.ini, 2.04 to 2.10) is the disc's, as at a start from the drive; a
# disc whose name the watcher doesn't know has no sheet of its own (the all-games one, and L1 stays there).
mkdir -p "$work/old-file/settings" && printf '[EmuCore/GS]\nupscale_multiplier=6\n' > "$work/old-file/settings/SLUS-20312.ini"
run old-file "game 0" "wait 1" "drive SLUS-20312 Final Fantasy X" "wait 0.8" "press left" "wait 0.8" "expect selected=-2" "press square" "wait 0.8" \
  "expectsheetfile settings/SLUS-20312.ini"
run no-serial "game 0" "wait 1" "drive - Unknown" "wait 0.8" "press left" "wait 0.8" "press square" "wait 0.8" "expectsheetfile gs.ini" \
  "press l1" "wait 0.5" "expectsheetfile gs.ini"
# fix180 (tester on 180: back from a disc game, the shelf showed the first entry): the disc played last (lastgame.txt "disc:<serial>") is picked when the
# watcher reports it; not when the player has moved meanwhile, and not for another disc.
run last-disc "game 2" "lastdisc SLUS-20312" "wait 1" "expect selected=2" "drive SLUS-20312 Final Fantasy X" "wait 0.8" "expect driveentry=1" \
  "expect selected=-2"
run last-disc-moved "game 2" "lastdisc SLUS-20312" "wait 1" "press right" "wait 0.5" "expect selected=3" "drive SLUS-20312 Final Fantasy X" "wait 0.8" \
  "expect driveentry=1" "expect selected=3"
run last-disc-other "game 2" "lastdisc SCES-50001" "wait 1" "drive SLUS-20312 Final Fantasy X" "wait 0.8" "expect driveentry=1" "expect selected=2"
# (review) The copy's entry selects itself, so it waits while a sheet is open: no jump of the pick under the sheet; it comes when the sheet closes.
run busy "game 1" "wait 1" "press square" "wait 0.8" "expect sheet=1" "dump 40" "wait 1.2" "expect dumppct=-1" "expect selected=1" "press circle" "wait 1.2" "expect sheet=0" "expect dumppct=40" "expect selected=-1"
# The disc is taken out while it is picked: the shelf closes the gap; another disc replaces it.
run swapped "game 0" "wait 1" "drive SLUS-20312 Final Fantasy X" "wait 0.8" "press left" "wait 0.8" "expect selected=-2" "drive SCES-50001 Another Disc" "wait 0.8" \
  "expect driveentry=1" "expect shelf=6" "drive off" "wait 0.8" "expect driveentry=0" "expect shelf=5" "expect selected=0"
# With the disc's art (covers/SLUS-00000_disc.png): the art arrives, on the drive entry and then on the copy's entry; no flip on either.
art art
run art "game 1" "wait 1" "drive SLUS-00000 Host Dump Game" "wait 0.5" "sleep 0.6" "wait 0.5" "expect driveentry=1" "expect discart=1" "press left 2" "wait 1" "shot drive-art" \
  "dump 30" "wait 1.2" "expect driveentry=0" "expect dumppct=30" "expect discart=1" "expect shelf=6" "shot dump-art" \
  "stick 1 0" "wait 1" "expect flip100=0" "stick 0 0" "dump off" "wait 1" "expect dumppct=-1" "expect driveentry=1" "expect discart=1" "expect shelf=6" \
  "drive off" "wait 0.5" "expect driveentry=0" "expect discart=1" "wait 3" "expect discart=0" "expect shelf=5"
# Without art the painted disc is drawn (and nothing is asked of the network).
run no-art "game 0" "wait 1" "drive SLUS-00000 Host Dump Game" "wait 1" "expect driveentry=1" "expect discart=0" "press left" "wait 1" "shot drive-painted"
# The art of one disc is not taken for another's.
art wrong-disc
run wrong-disc "game 0" "wait 1" "drive SCES-50001 Another Disc" "wait 0.5" "sleep 0.6" "wait 0.5" "expect driveentry=1" "expect discart=0"
# (coverfit) The disc is drawn as a circle of one size on the case, whatever the scan was: art() above is a plain 700x700 disc; an elliptical 900x700 scan and
# a 1000x1000 one with a wide transparent margin must come out as round and as large (the picture is cut to the disc and made square when it is decoded).
art_shape() { # name width height margin (the transparent surround, as a fraction of each side)
  mkdir -p "$work/$1/covers"
  python3 - "$work/$1/covers/SLUS-00000_disc.png" "$2" "$3" "$4" <<'PY'
import sys
from PIL import Image, ImageDraw
path, w, h, margin = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), float(sys.argv[4])
im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
d = ImageDraw.Draw(im)
x0, y0 = w * margin, h * margin
dw, dh = w - 2 * x0, h - 2 * y0
def ring(f, fill):  # an ellipse f (a fraction of the disc) in from each side
    d.ellipse([x0 + dw * f, y0 + dh * f, x0 + dw * (1 - f) - 1, y0 + dh * (1 - f) - 1], fill=fill)
ring(0, (236, 232, 246, 255))
ring(40 / 700, (40, 70, 150, 255))
ring(120 / 700, (230, 170, 60, 255))
d.rectangle([x0 + dw * (0.5 - 30 / 700), y0 + dh * 60 / 700, x0 + dw * (0.5 + 30 / 700), y0 + dh * 150 / 700], fill=(220, 30, 30, 255))
ring(0.5 - 90 / 700, (236, 232, 246, 255))
ring(0.5 - 40 / 700, (0, 0, 0, 0))
im.save(path)
PY
}
art_shape art-ellipse 900 700 0
run art-ellipse "game 1" "wait 1" "drive SLUS-00000 Host Dump Game" "wait 0.5" "sleep 0.6" "wait 0.5" "expect driveentry=1" "expect discart=1" "press left 2" "wait 1" "shot drive-ellipse"
art_shape art-margin 1000 1000 0.15
run art-margin "game 1" "wait 1" "drive SLUS-00000 Host Dump Game" "wait 0.5" "sleep 0.6" "wait 0.5" "expect driveentry=1" "expect discart=1" "press left 2" "wait 1" "shot drive-margin"
# The orange ring of the label, measured in the three pictures: round (width / height 1 +/- 3 %) and the same size in all three (+/- 4 %).
python3 - "$shots" <<'PY' || { echo "FAILED: disc size and shape"; failed=1; }
import sys
from PIL import Image
sizes = {}
for name in ("drive-art", "drive-ellipse", "drive-margin"):
    im = Image.open(sys.argv[1] + "/" + name + ".png").convert("RGB")
    px = im.load()
    x0, y0, x1, y1 = im.width, im.height, -1, -1
    for y in range(im.height):
        for x in range(im.width):
            r, g, b = px[x, y]
            if abs(r - 230) < 20 and abs(g - 170) < 20 and abs(b - 60) < 25:
                x0, x1, y0, y1 = min(x0, x), max(x1, x), min(y0, y), max(y1, y)
    if x1 < x0:
        sys.exit("%s: the label is not in the picture" % name)
    w, h = x1 - x0 + 1, y1 - y0 + 1
    print("%-14s label %dx%d (w/h %.3f)" % (name, w, h, w / h))
    if not 0.97 <= w / h <= 1.03:
        sys.exit("%s: the disc is not round" % name)
    sizes[name] = w
if max(sizes.values()) > min(sizes.values()) * 1.04:
    sys.exit("the three discs differ in size: %s" % sizes)
PY
exit $failed
