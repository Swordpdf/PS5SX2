#!/bin/bash
# Builds PS5SX2-Disc.elf: the disc tile payload (disctile.c) AND the disc watcher (daemon/disc_launch.c, built embedded) in ONE ELF.
# Needs: the PS5 payload SDK (prospero-clang), LLVM 22 for its lld, mbedTLS 3.6 sources (deps/mbedtls: the PS5SX2 repo's ps5/third_party/mbedtls) and a
# PS5 build of SQLite 3.45 (deps/sqlite-ps5: libsqlite3.a and sqlite3.h). Paths can be given in the environment:
#   PS5_PAYLOAD_SDK  (default ~/rps5/toolchain/ps5-payload-sdk)    LLVM_BIN (default ~/rps5/toolchain/LLVM-22.1.8-Linux-X64/bin)
#   MBEDTLS_DIR      (default ../deps/mbedtls, else the PS5SX2 tree)   SQLITE_DIR (default ../deps/sqlite-ps5, else ~/sqlite-ps5)
#   BUILD_DIR        (default ~/rps5/discbuild)
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
MB=${MBEDTLS_DIR:-}
[ -z "$MB" ] && { [ -d "$HERE/../deps/mbedtls" ] && MB="$HERE/../deps/mbedtls" || MB=$HERE/../third_party/mbedtls; [ -d "$MB" ] || MB=$HOME/ps5sx2-build/PS5SX2/ps5/third_party/mbedtls; }
SQLITE=${SQLITE_DIR:-}
[ -z "$SQLITE" ] && { [ -d "$HERE/../deps/sqlite-ps5" ] && SQLITE="$HERE/../deps/sqlite-ps5" || SQLITE=$HOME/sqlite-ps5; }
W=${BUILD_DIR:-$HOME/rps5/discbuild}; mkdir -p $W/obj && cd $W
for f in disctile.c tile.c tile.h; do sed 's/\r$//' "$HERE/$f" > $f; done
for f in stb_image.h stb_truetype.h; do sed 's/\r$//' "$(stbsrc $f)" > $f; done
for f in disc_launch.c disc_hash.c disc_hash.h disc_switch.h disc_fastread.h disc_compat.h disc_compat_db.h; do sed 's/\r$//' "$DAEMON/$f" > $f; done
for f in sgdb_art.cpp fe_https.cpp fe_https_ps5.cpp fe_https.h fe_https_roots.inc; do sed 's/\r$//' "$HERE/https/$f" > $f; done
for f in kp.c kp.h; do sed 's/\r$//' "$HERE/$f" > $f; done
CC=$SDK/bin/prospero-clang; CXX=$SDK/bin/prospero-clang++
printf '%s\n' "-DMBEDTLS_CONFIG_FILE=\\\"ps5sx2_mbedtls_config.h\\\"" "-I$MB/port" "-I$MB/include" "-I$MB/library" > mb.rsp
grep -o 'MBEDTLS_FILES += .*' $MB/port/files.mk | sed 's/MBEDTLS_FILES += //' > mbfiles.txt
if [ ! -f obj/mb_aes.c.o ] && [ -f $HOME/rps5/tile6build/obj/mb_aes.c.o ]; then cp $HOME/rps5/tile6build/obj/mb_* obj/; fi
if [ ! -f obj/mb_aes.c.o ]; then
  echo "compiling $(wc -l < mbfiles.txt) mbedTLS files ..."
  while read f; do echo "$f"; done < mbfiles.txt | xargs -P 8 -I{} sh -c "$CC -std=gnu11 -O2 -DNDEBUG @mb.rsp -include string.h -include stdlib.h -c $MB/library/{} -o obj/mb_{}.o 2>&1 | grep -v 'no version information' | head -5"
  $CC -std=gnu11 -O2 -DNDEBUG @mb.rsp -include string.h -include stdlib.h -c $MB/port/ps5sx2_mbedtls_platform.c -o obj/mb_platform.o 2>&1 | grep -v 'no version information' | head -5
fi
CXXF="-std=c++20 -O2 -DNDEBUG -fno-exceptions -fno-rtti @mb.rsp -I."
for f in sgdb_art fe_https fe_https_ps5; do $CXX $CXXF -c $f.cpp -o obj/$f.o 2>&1 | grep -v 'no version information' | head -20; done
$CC -std=gnu11 -O2 -w -c tile.c -o obj/tile.o 2>&1 | grep -v 'no version information' | head -20
$CC -std=gnu11 -O2 -Wall -Wextra -Werror -c kp.c -o obj/kp.o 2>&1 | grep -v 'no version information' | head -40
$CC -std=gnu11 -O2 -Wall -Wextra -Werror -DDISC_AUTO_EMBEDDED -c disc_launch.c -o obj/disc_launch.o 2>&1 | grep -v 'no version information' | head -40
$CC -std=gnu11 -O2 -Wall -Wextra -Werror -c disc_hash.c -o obj/disc_hash.o 2>&1 | grep -v 'no version information' | head -40
$CC -std=gnu11 -O2 -Wall -Wextra -Werror -DDISC_MERGED -I$SQLITE -c disctile.c -o obj/disctile.o 2>&1 | grep -v 'no version information' | head -40
$CXX -o PS5SX2-Disc.elf obj/*.o -L$SQLITE -lsqlite3 -L$SDK/target/lib -lSceNet -lpthread 2>&1 | grep -v 'no version information' | head -30
ls -la PS5SX2-Disc.elf && cp PS5SX2-Disc.elf "$HERE/PS5SX2-Disc.elf"
