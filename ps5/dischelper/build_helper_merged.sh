#!/bin/bash
# Builds the MERGED helper: PS5SXHelper.elf = the helper's host (helper-host/: main.c, the /data mount thread mount.c, the jailbreak watcher jb.c; GLM's files,
# copied unchanged) + this disc payload (disctile.c built with -DDISC_HOST_THREAD, so its entry is disc_payload_thread(), + the disc watcher
# daemon/disc_launch.c built embedded) in ONE process. The standalone PS5SX2-Disc.elf (build_disc.sh) is the same payload as a process of its own; never load
# both, or both act on every disc.
# Helper 171f (2026-10-16): the DEFAULT build is the configuration the tester ran without a kernel panic (see HELPER_LOGS, KP below) plus game quiet and the
# symbol strip; nothing needs to be set for the ELF that is sent to a tester.
# Helper 171g (2026-10-16): the same, plus game tiles (gametile.c, included by disctile.c in this hosted build only; OFF unless the flag file
# /data/PCSX2/flags/gametiles exists). Two more things happen here: the build folder's COPY of jb.c gets the ten game-tile title IDs PPSA99251..PPSA99260 in its
# built-in list (an exact-text patch; the build stops when the anchor is not there exactly once), and after the link the undefined-symbol list of the ELF must be
# the one of the 171f ELF (imports-171f.txt: a new libc import could stop the console's payload loader from starting the WHOLE helper).
#   VARIANT=normal   (default) the helper with its /data mount thread -> PS5SXHelper-merged.elf. Needed with ShadowMount+ 1.7beta3, which does not mount /data.
#   VARIANT=nomount  main-nomount.c instead of main.c, and no mount.c: no /data mount thread -> PS5SXHelper-merged-nomount.elf. For ShadowMount+ 1.7-beta5 and
#                    later, which mounts /data into the sandbox of the titles it manages by itself and takes it off when the game stops (the helper's own mount
#                    then lies on top of it as a second layer and is never taken off: the stale sandboxes).
# Needs the same things as build_disc.sh (PS5 payload SDK with prospero-clang, LLVM for its lld, mbedTLS sources in ../deps/mbedtls, SQLite for PS5 in
# ../deps/sqlite-ps5). Paths can be given in the environment:
#   PS5_PAYLOAD_SDK  (default ~/rps5/toolchain/ps5-payload-sdk)    LLVM_BIN (default ~/rps5/toolchain/LLVM-22.1.8-Linux-X64/bin)
#   MBEDTLS_DIR      (default ../deps/mbedtls, else the PS5SX2 tree)   SQLITE_DIR (default ../deps/sqlite-ps5, else ~/sqlite-ps5)
#   HOST_DIR         (default ./helper-host)    BUILD_DIR (default ~/rps5/helperbuild-merged, or ~/rps5/helperbuild-nomount; a copy of this folder that is not named tile6 uses
#                    ~/rps5/helperbuild-<copy's name without "tile6-">-merged / -nomount, e.g. tile6-tiles -> helperbuild-tiles-merged)
VARIANT=${VARIANT:-normal}
LLVM_BIN=${LLVM_BIN:-$HOME/rps5/toolchain/LLVM-22.1.8-Linux-X64/bin}
export PATH=$LLVM_BIN:$HOME/.local/bin:$PATH
export LLVM_CONFIG=$LLVM_BIN/llvm-config
export LD_LIBRARY_PATH=$HOME/rps5/shimlib:${LD_LIBRARY_PATH:-}
SDK=${PS5_PAYLOAD_SDK:-$HOME/rps5/toolchain/ps5-payload-sdk}
HERE="$(cd "$(dirname "$0")" && pwd)"
# In the tree (ps5/dischelper): the disc watcher's files are in ../disclaunch and stb in ../frontend/third_party; a copy laid out
# the old way (tile6: daemon/ and the stb headers beside this script) still builds.
DAEMON=$HERE/daemon; [ -d "$DAEMON" ] || DAEMON=$HERE/../disclaunch
stbsrc() { [ -f "$HERE/$1" ] && echo "$HERE/$1" || echo "$HERE/../frontend/third_party/$1"; }
HOST=${HOST_DIR:-$HERE/helper-host}
MB=${MBEDTLS_DIR:-}
[ -z "$MB" ] && { [ -d "$HERE/../deps/mbedtls" ] && MB="$HERE/../deps/mbedtls" || MB=$HERE/../third_party/mbedtls; [ -d "$MB" ] || MB=$HOME/ps5sx2-build/PS5SX2/ps5/third_party/mbedtls; }
SQLITE=${SQLITE_DIR:-}
[ -z "$SQLITE" ] && { [ -d "$HERE/../deps/sqlite-ps5" ] && SQLITE="$HERE/../deps/sqlite-ps5" || SQLITE=$HOME/sqlite-ps5; }
# 171g: a COPY of this folder (one not named tile6) builds in build folders of its own, named after the copy (tile6-tiles -> helperbuild-tiles-merged / -nomount): a bare run of the copy's script
# must not overwrite the build folders of the real tile6 (helperbuild-merged / helperbuild-nomount: the 171f links and objects of the real tree, and the references of the machine-code
# comparisons). The real tile6 keeps the old names. BUILD_DIR still wins.
BNAME=$(basename "$HERE"); BNAME=${BNAME#tile6}; BNAME=${BNAME#-}
BTAG=""; [ -n "$BNAME" ] && BTAG="-$BNAME"
case "$VARIANT" in
  nomount) W=${BUILD_DIR:-$HOME/rps5/helperbuild${BTAG}-nomount}; OUT=PS5SXHelper-merged-nomount.elf; MAIN=main-nomount.c ;;
  normal)  W=${BUILD_DIR:-$HOME/rps5/helperbuild${BTAG}-merged};  OUT=PS5SXHelper-merged.elf;         MAIN=main.c ;;
  *) echo "VARIANT must be normal or nomount"; exit 1 ;;
esac
#   The defaults are the configuration the tester ran without a kernel panic (helper 171e "KP test", PS5SX2 Disc Helper - KP test.elf) plus 171f's additions; the
#   quiet-at-shutdown watch itself (kp.c) is in every build (flag kp_off_quiesce turns it off at run time).
#   HELPER_LOGS=0    (default) the payload writes no log file unless the flag kp_log exists (its lines go to the klog; payload-init.log keeps the start-up lines).
#                    HELPER_LOGS=1: a developer build that always writes its log files (disctile.log, kernel-log.txt, shadowmount-debug.log, disc-auto.log);
#                    "-logs" is added to its name and build folder.
#   KP=1             (default) the host's jb.c and mount.c (copied unchanged from GLM's files, patched here) stop their polling while the console shuts down or rests
#                    (kp.c); the build stops when GLM's files are not the ones the patch expects. KP=0: GLM's files unpatched ("-plain" in the name and folder).
#   STRIP=1          (default) the symbol table is taken out after the link (llvm-strip --strip-all), and strip_gate.py checks that the program headers, every
#                    byte the loader maps, the allocated sections and the import list are what the link gave; the build stops when they are not. The unstripped link
#                    stays in the build folder as PS5SXHelper.unstripped.elf. STRIP=0 ships the unstripped link (symbols for a crash analysis).
HELPER_LOGS=${HELPER_LOGS:-0}
KP=${KP:-1}
STRIP=${STRIP:-1}
for sw in HELPER_LOGS KP STRIP; do case "${!sw}" in 0|1) ;; *) echo "$sw must be 0 or 1"; exit 1 ;; esac; done
KPDEF=""
SUF=""
[ "$HELPER_LOGS" = 0 ] && KPDEF="-DKP_TEST_NOLOG=1"
[ "$HELPER_LOGS" = 1 ] && SUF="$SUF-logs"
[ "$KP" = 0 ] && SUF="$SUF-plain"
W=${W}${SUF}; OUT=${OUT%.elf}${SUF}.elf
mkdir -p $W/obj && cd $W
rm -f obj/main.o obj/mount.o obj/jb.o obj/disctile.o obj/tile.o obj/disc_launch.o obj/disc_hash.o obj/kp.o
sed 's/\r$//' "$HOST/$MAIN" > main.c
for f in jb.c jb.h progress.h; do sed 's/\r$//' "$HOST/$f" > $f; done
for f in mount.c mount.h; do sed 's/\r$//' "$HOST/$f" > $f; done   # kept in the folder for both variants; compiled for "normal" only
for f in disctile.c gametile.c tile.c tile.h; do sed 's/\r$//' "$HERE/$f" > $f; done
for f in stb_image.h stb_truetype.h; do sed 's/\r$//' "$(stbsrc $f)" > $f; done
for f in disc_launch.c disc_hash.c disc_hash.h disc_switch.h disc_fastread.h disc_speed8.c disc_speed8.h disc_compat.h disc_compat_db.h; do sed 's/\r$//' "$DAEMON/$f" > $f; done
for f in sgdb_art.cpp fe_https.cpp fe_https_ps5.cpp fe_https.h fe_https_roots.inc; do sed 's/\r$//' "$HERE/https/$f" > $f; done
for f in kp.c kp.h; do sed 's/\r$//' "$HERE/$f" > $f; done
# 171g: the game tiles' title IDs go into the BUILT-IN list of the copy of jb.c (the jailbreak watcher gives them root like the disc tiles'; gametile.c also appends them to
# /data/whitelist.txt as a second route). Independent of KP; exact text: the build stops when the anchor is not there exactly once.
python3 - <<'PYEOF'
anchor = '"PPSA99235", "PPSA99236", "PPSA99237", "PPSA99238",'
add = ('\n    /* the game tiles\' slots (gametile.c, helper 171g) */\n'
       '    "PPSA99251", "PPSA99252", "PPSA99253", "PPSA99254", "PPSA99255",\n'
       '    "PPSA99256", "PPSA99257", "PPSA99258", "PPSA99259", "PPSA99260",')
t = open("jb.c").read()
assert t.count(anchor) == 1, ("jb.c: the built-in title list is not the one the game-tile patch expects", t.count(anchor))
assert "PPSA99251" not in t, "jb.c already names the game tiles"
t = t.replace(anchor, anchor + add)
assert all(t.count('"PPSA992%d"' % n) == 1 for n in range(51, 61)), "jb.c: the ten game-tile IDs are not there exactly once each"
open("jb.c", "w").write(t)
print("game-tile IDs PPSA99251..PPSA99260 added to the built-in list of jb.c")
PYEOF
[ $? = 0 ] || { echo "BUILD FAILED: the jb.c game-tile patch"; exit 1; }
if [ "$KP" = 1 ]; then # the host's two polling threads go quiet while the console shuts down (exact-text patches of the copies: the build stops when a file is not the one expected)
python3 - <<'PYEOF'
def patch(path, old, new, after):
    t = open(path).read()
    assert t.count(old) == 1 and t.count(after) == 1, (path, "the expected text is not there")
    open(path, "w").write(t.replace(old, new).replace(after, after + '\n#include "kp.h"'))
patch("jb.c", "  for (;;) {\n    DIR *d = opendir(SANDBOX_BASE);", "  for (;;) {\n    if (kp_quiet()) { usleep(POLL_US); continue; }\n    DIR *d = opendir(SANDBOX_BASE);", '#include "progress.h"')
patch("mount.c", "    for (;;) {\n        DIR *directory = opendir(SANDBOX_BASE);", "    for (;;) {\n        if (kp_quiet()) { usleep(MOUNT_POLL_US * 4); continue; }\n        DIR *directory = opendir(SANDBOX_BASE);", '#include "progress.h"')
print("kp patches applied to jb.c and mount.c")
PYEOF
[ $? = 0 ] || { echo "BUILD FAILED: a kp patch (the host's files are not the ones expected)"; exit 1; } # (171g: the comment above said so, but a failed patch let the script go on)
fi
CC=$SDK/bin/prospero-clang; CXX=$SDK/bin/prospero-clang++
printf '%s\n' "-DMBEDTLS_CONFIG_FILE=\\\"ps5sx2_mbedtls_config.h\\\"" "-I$MB/port" "-I$MB/include" "-I$MB/library" > mb.rsp
grep -o 'MBEDTLS_FILES += .*' $MB/port/files.mk | sed 's/MBEDTLS_FILES += //' > mbfiles.txt
# the mbedTLS objects are the same for every build here: taken from the standalone build's folder when it has them
if [ ! -f obj/mb_aes.c.o ] && [ -f $HOME/rps5/discbuild/obj/mb_aes.c.o ]; then cp $HOME/rps5/discbuild/obj/mb_* obj/; fi
if [ ! -f obj/mb_aes.c.o ]; then
  echo "compiling $(wc -l < mbfiles.txt) mbedTLS files ..."
  while read f; do echo "$f"; done < mbfiles.txt | xargs -P 8 -I{} sh -c "$CC -std=gnu11 -O2 -DNDEBUG @mb.rsp -include string.h -include stdlib.h -c $MB/library/{} -o obj/mb_{}.o 2>&1 | grep -v 'no version information' | head -5"
  $CC -std=gnu11 -O2 -DNDEBUG @mb.rsp -include string.h -include stdlib.h -c $MB/port/ps5sx2_mbedtls_platform.c -o obj/mb_platform.o 2>&1 | grep -v 'no version information' | head -5
fi
CXXF="-std=c++20 -O2 -DNDEBUG -fno-exceptions -fno-rtti @mb.rsp -I."
for f in sgdb_art fe_https fe_https_ps5; do $CXX $CXXF -c $f.cpp -o obj/$f.o 2>&1 | grep -v 'no version information' | head -20; done
$CC -std=gnu11 -O2 -w -c tile.c -o obj/tile.o 2>&1 | grep -v 'no version information' | head -20
$CC -std=gnu11 -O2 -Wall -Wextra -Werror $KPDEF -c kp.c -o obj/kp.o 2>&1 | grep -v 'no version information' | head -40
$CC -std=gnu11 -O2 -Wall -Wextra -Werror -DDISC_AUTO_EMBEDDED -c disc_launch.c -o obj/disc_launch.o 2>&1 | grep -v 'no version information' | head -40
$CC -std=gnu11 -O2 -Wall -Wextra -Werror -DDISC_AUTO_EMBEDDED -c disc_speed8.c -o obj/disc_speed8.o 2>&1 | grep -v 'no version information' | head -40 # 179: DiscSpeed8 SceShellCore 8x patch + 60 s drive pin
$CC -std=gnu11 -O2 -Wall -Wextra -Werror -c disc_hash.c -o obj/disc_hash.o 2>&1 | grep -v 'no version information' | head -40
$CC -std=gnu11 -O2 -Wall -Wextra -Werror -DDISC_MERGED -DDISC_HOST_THREAD -I$SQLITE -c disctile.c -o obj/disctile.o 2>&1 | grep -v 'no version information' | head -40
$CC -std=gnu11 -O2 -Wall -Wextra -Werror -c main.c -o obj/main.o 2>&1 | grep -v 'no version information' | head -40
[ "$VARIANT" = normal ] && $CC -std=gnu11 -O2 -Wall -Wextra -Werror -c mount.c -o obj/mount.o 2>&1 | grep -v 'no version information' | head -40
$CC -std=gnu11 -O2 -Wall -Wextra -Werror -c jb.c -o obj/jb.o 2>&1 | grep -v 'no version information' | head -40
rm -f PS5SXHelper.elf PS5SXHelper.unstripped.elf # a failed compile or link must not ship an ELF of an earlier build
$CXX -o PS5SXHelper.elf obj/*.o -L$SQLITE -lsqlite3 -L$SDK/target/lib -lSceNet -lpthread 2>&1 | grep -v 'no version information' | head -30
[ -f PS5SXHelper.elf ] || { echo "BUILD FAILED: no PS5SXHelper.elf was linked"; exit 1; }
if [ "$STRIP" = 1 ]; then
  cp PS5SXHelper.elf PS5SXHelper.unstripped.elf
  "$LLVM_BIN/llvm-strip" --strip-all PS5SXHelper.elf || { echo "BUILD FAILED: llvm-strip"; rm -f PS5SXHelper.elf; exit 1; }
  python3 "$HERE/strip_gate.py" PS5SXHelper.unstripped.elf PS5SXHelper.elf || { echo "BUILD FAILED: the stripped ELF is not the unstripped one for the loader"; rm -f PS5SXHelper.elf; exit 1; }
  # the same two facts by LLVM's own tools: the program header table, and the dynamic imports (the list every payload loader resolves)
  gate_phdrs() { "$LLVM_BIN/llvm-readelf" -lW "$1" 2>/dev/null | sed -n '/Program Headers/,/Section to Segment/p'; }
  gate_imports() { "$LLVM_BIN/llvm-nm" -D --undefined-only "$1" 2>/dev/null | sort; }
  for g in gate_phdrs gate_imports; do
    cmp -s <($g PS5SXHelper.unstripped.elf) <($g PS5SXHelper.elf) || { echo "BUILD FAILED: $g differs between the stripped and the unstripped link"; rm -f PS5SXHelper.elf; exit 1; }
    echo "gate: $g is the same before and after the strip ($($g PS5SXHelper.elf | wc -l) lines)"
  done
fi
# 171g gates on the ELF that is about to be shipped (also with STRIP=0):
#  (a) its undefined dynamic symbols are exactly those of the 171f ELF (imports-171f.txt, made with this script's llvm-nm from the shipped 171f ELF): the payload loader of the
#      console resolves every one of them against its libraries, and one it does not find stops the WHOLE helper, the disc tiles with it. IMPORTS_BASELINE=none: no comparison
#      (a deliberate new import); a developer variant (HELPER_LOGS=1, KP=0) that differs on purpose is built that way too.
#  (b) the ten game-tile IDs are in the built-in title list of jb.o, and the first and the last are in the ELF.
IMPORTS_BASELINE=${IMPORTS_BASELINE:-$HERE/imports-171f.txt}
if [ "$IMPORTS_BASELINE" != none ]; then
  [ -f "$IMPORTS_BASELINE" ] || { echo "BUILD FAILED: $IMPORTS_BASELINE (the import list of the 171f ELF) is missing"; rm -f PS5SXHelper.elf; exit 1; }
  imports_now() { "$LLVM_BIN/llvm-nm" -D --undefined-only PS5SXHelper.elf 2>/dev/null | sort; }
  cmp -s <(imports_now) "$IMPORTS_BASELINE" || { echo "BUILD FAILED: the undefined dynamic symbols differ from the 171f ELF's ($IMPORTS_BASELINE):"; diff "$IMPORTS_BASELINE" <(imports_now) | head -20; rm -f PS5SXHelper.elf; exit 1; }
  echo "gate: the undefined dynamic symbols are the 171f ELF's ($(wc -l < "$IMPORTS_BASELINE") lines)"
fi
for n in 51 52 53 54 55 56 57 58 59 60; do
  strings -a obj/jb.o | grep -q -F "PPSA992$n" || { echo "BUILD FAILED: PPSA992$n is not in the built-in list of jb.o"; rm -f PS5SXHelper.elf; exit 1; }
done
for n in 51 60; do
  strings -a PS5SXHelper.elf | grep -q -F "PPSA992$n" || { echo "BUILD FAILED: PPSA992$n is not in the ELF"; rm -f PS5SXHelper.elf; exit 1; }
done
echo "gate: PPSA99251..PPSA99260 are in jb.o's built-in list and in the ELF"
ls -la PS5SXHelper.elf && sha256sum PS5SXHelper.elf && cp PS5SXHelper.elf "$HERE/$OUT"
echo "built $OUT: helper 171k (next to the PS5SX3 helper: non-PS2 discs yielded, flag disc_noyield; DiscSpeed8 8x shell patch + 60 s drive pin for PS2 discs, flag disc_nospeed8), game tiles ON by default (flag gametile_off switches off), log files $([ "$HELPER_LOGS" = 1 ] && echo ON || echo "off (flag kp_log)"), jb/mount patches $([ "$KP" = 1 ] && echo ON || echo off), symbols $([ "$STRIP" = 1 ] && echo stripped || echo kept)"
