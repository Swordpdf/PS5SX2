#!/usr/bin/env bash
# PS5 port frontend: the HD texture pack tests (texpacks_test.cpp) on a PC (2026-10-05, AI-assisted). Builds the vendored
# libarchive (ps5/third_party/libarchive, its Linux config), fe_texpacks.cpp and fe_settings.cpp with the sanitizers,
# makes the zip fixtures with Python, and runs the test. TEXPACKS_REAL="<a pack .rar>:<serial>" also unpacks a real
# archive.org pack through the manager.
#
#   ps5/frontend/host/test-texpacks.sh
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fe=$(cd -- "$here/.." && pwd)
pcsx2=$(cd -- "$fe/../.." && pwd)
la="$pcsx2/ps5/third_party/libarchive"
CXX=${CXX:-clang++-18}
CC=${CC:-clang-18}
work=$(mktemp -d "${TMPDIR:-/tmp}/texpacks.XXXXXX")
trap 'rm -rf "$work"' EXIT
# TEXPACKS_SAN="-fno-omit-frame-pointer" builds without the sanitizers (a machine without their runtimes)
san=(${TEXPACKS_SAN:--fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer})

mkdir -p "$work/obj"
objs=()
for c in "$la"/libarchive/*.c; do
  o="$work/obj/$(basename "$c" .c).o"
  "$CC" -O1 -g -w -DHAVE_CONFIG_H -I"$la" -I"$la/libarchive" "${san[@]}" -c "$c" -o "$o"
  objs+=("$o")
done
# 2026-10-08: fe_settings.cpp's ManualFixSettings needs fe_games.cpp's GameDbHwFixes (vk-285-132), and fe_games.cpp what the
# hardware fixes test links with it (the image readers' lz4 and the libchdr stand-in).
"$CC" -O1 -g -c "$pcsx2/ps5/third_party/lz4/lz4.c" -o "$work/obj/lz4.o"
"$CC" -O1 -g -I"$pcsx2/3rdparty/libchdr/include" -c "$here/chd_stub.c" -o "$work/obj/chd_stub.o"
# b176: the 7z reader (fe_sevenzip.c) and the LZMA SDK files it needs: 14 of 3rdparty/lzma/src (this harness links chd_stub.c, not libchdr, so LzmaDec.c is built
# here too; Makefile.vk has it through CHD_CSRCS). The SDK reads its headers with unaligned 64-bit loads: the alignment check stays off for its objects.
sdk="$pcsx2/3rdparty/lzma"
for s in 7zArcIn 7zBuf 7zCrc 7zCrcOpt 7zDec 7zStream Bcj2 Bra Bra86 BraIA64 CpuArch Delta Lzma2Dec LzmaDec; do
  "$CC" -O1 -g -w -DZ7_ST -I"$sdk/include" "${san[@]}" -fno-sanitize=alignment -c "$sdk/src/$s.c" -o "$work/obj/sz_$s.o"
  objs+=("$work/obj/sz_$s.o")
done
"$CC" -O1 -g -Wall -Wextra -Wno-unused-parameter -DZ7_ST -I"$sdk/include" "${san[@]}" -c "$fe/fe_sevenzip.c" -o "$work/obj/fe_sevenzip.o"
objs+=("$work/obj/fe_sevenzip.o")
"$CXX" -std=c++20 -O1 -g -Wall -Wno-unused-function -I"$fe" -I"$pcsx2/pcsx2" -I"$la/libarchive" -I"$pcsx2/3rdparty/rapidjson/include" \
  -I"$pcsx2/3rdparty/vulkan/include" -I"$pcsx2/3rdparty/libchdr/include" -I"$pcsx2/ps5/third_party/lz4" "${san[@]}" \
  "$here/texpacks_test.cpp" "$fe/fe_texpacks.cpp" "$fe/fe_texwrite.cpp" "$fe/fe_settings.cpp" "$fe/fe_games.cpp" "$fe/fe_covers.cpp" "$fe/fe_text.cpp" \
  "$fe/fe_i18n.cpp" "${objs[@]}" "$work/obj/lz4.o" "$work/obj/chd_stub.o" -lz -lpthread -o "$work/texpacks_test"

# 2026-10-29: the texture pack writer payload (ps5/coreorbis/tex-writer/tex_writer.c), built for the PC: the tests start it as a process of its own,
# the way the console's ELF loader does, and the manager writes the packs through it (TEX_WRITER_BIN).
"$CC" -O1 -g -Wall -Wextra -Werror -I"$fe" "$pcsx2/ps5/coreorbis/tex-writer/tex_writer.c" -o "$work/tex_writer"

# The fixtures: a pack laid out as archive.org's are (with the maker's dumps/ and a readme beside it), and a two-disc one.
mkdir -p "$work/fixtures" "$work/run"
python3 - "$work/fixtures" <<'EOF'
import hashlib, os, sys, zipfile
d = sys.argv[1]
def make(name, files):
    p = os.path.join(d, name)
    with zipfile.ZipFile(p, "w", zipfile.ZIP_DEFLATED) as z:
        for path, data in files:
            z.writestr(path, data)
    return hashlib.md5(open(p, "rb").read()).hexdigest()
pack = "Spider-Man 2 (USA) [SLUS-20776] HD Remaster"
good = make(pack + ".zip", [
    (pack + "/SLUS-20776/replacements/a.dds", os.urandom(3 * 1024 * 1024)),
    (pack + "/SLUS-20776/replacements/01/b.png", b"the second texture"),
    (pack + "/SLUS-20776/dumps/c.png", b"a dump"),
    (pack + "/SLUS-20776/replacements.pak", b"not a pack: the pack file's place is the unpacker's"),
    (pack + "/SLUS-20776/replacements/01/b.png", b"the second texture, again"),
    ("readme.txt", b"thanks"),
])
disc = "Onimusha (USA) (Disc 1) [SLUS-21180] & (Disc 2) [SLUS-21362] HD Remaster"
two = make(disc + ".zip", [
    (disc + "/SLUS-21180/replacements/d1.png", b"disc one"),
    (disc + "/SLUS-21362/replacements/d2.png", b"disc two"),
])
# Many files (one pack file of 3,004), and a file outside replacements/ that lands where a folder already is (a failed write).
many_name = "Many Files (USA) [SLUS-11111] HD Remaster"
files = []
for i in range(3000):
    files.append((many_name + "/SLUS-11111/replacements/%02d/t%04d.png" % (i % 30, i), ("texture %d " % i).encode() * (1 + i % 50)))
for i in range(4):
    files.append((many_name + "/SLUS-11111/replacements/big/b%d.dds" % i, os.urandom(5 * 1024 * 1024)))
many = make(many_name + ".zip", files)
clash_name = "Clash (USA) [SLUS-22222] HD Remaster"
clash = make(clash_name + ".zip", [
    (clash_name + "/SLUS-22222/replacements/e.png", b"a texture"),
    (clash_name + "/SLUS-22222/notes/e.txt", b"inside"),
    (clash_name + "/SLUS-22222/notes", b"a file where the folder is"),
])
# 2.11: a pack for a Google Drive file (no md5 in any list): drive-only.zip
make("drive-only.zip", [("Drive Only/SLUS-33333/replacements/x.png", b"a drive texture")])
# 2.11: a pack whose entries run across the pack writer's 8 MiB buffers (exactly 8 MiB, one byte over, 20 MiB, one byte, none, 3 MiB x 3 after them):
# big.zip, and big.manifest ("<name under replacements/> <size> <md5>") for the test to check every entry by
import hashlib
def blob(seed, n):
    out = bytearray()
    i = 0
    while len(out) < n:
        out += hashlib.sha256(seed + i.to_bytes(4, "big")).digest()
        i += 1
    return bytes(out[:n])
big = [("a8.bin", 8 << 20), ("b8p1.bin", (8 << 20) + 1), ("c20.bin", 20 << 20), ("d1.bin", 1), ("e0.bin", 0), ("f3a.bin", 3 << 20), ("f3b.bin", 3 << 20), ("f3c.bin", 3 << 20)]
entries = [("Big/SLUS-77777/replacements/" + n, blob(n.encode(), size)) for n, size in big]
make("big.zip", entries)
with open(os.path.join(d, "big.manifest"), "w") as m:
    for (n, size), (_, data) in zip(big, entries):
        m.write("%s %d %s\n" % (n, size, hashlib.md5(data).hexdigest()))
open(os.path.join(d, "md5.txt"), "w").write(" ".join([good, two, many, clash]) + "\n")
EOF

# b176: the .7z fixtures (make_7z_fixtures.py texpacks: Python's lzma module and a 7z writer; nothing big is kept in the repository)
python3 -I -X utf8 "$here/make_7z_fixtures.py" texpacks "$work/fixtures" > "$work/fixtures-7z.log"

real=()
if [[ -n ${TEXPACKS_REAL:-} ]]; then
  real=("${TEXPACKS_REAL%:*}" "${TEXPACKS_REAL##*:}")
fi
TEX_WRITER_BIN="$work/tex_writer" HD_PACK_SOURCES="$fe/assets/hd-pack-sources.json" ASAN_OPTIONS=detect_leaks=1 "$work/texpacks_test" "$work/fixtures" "$work/run" "${real[@]}"
