#!/bin/bash
# Builds DiscTile.elf (WSL; needs the PS5 payload SDK in ~/rps5, the PS5SX2 sources in ~/ps5sx2-build for mbedTLS, and the sqlite library in ~/sqlite-ps5).
export PATH=$HOME/rps5/toolchain/LLVM-22.1.8-Linux-X64/bin:$HOME/.local/bin:$PATH
export LLVM_CONFIG=$HOME/rps5/toolchain/LLVM-22.1.8-Linux-X64/bin/llvm-config
export LD_LIBRARY_PATH=$HOME/rps5/shimlib:${LD_LIBRARY_PATH:-}
SDK=$HOME/rps5/toolchain/ps5-payload-sdk
MB=$HOME/ps5sx2-build/PS5SX2/ps5/third_party/mbedtls
HERE="$(cd "$(dirname "$0")" && pwd)"
# In the tree (ps5/dischelper): the disc watcher's files are in ../disclaunch and stb in ../frontend/third_party; a copy laid out
# the old way (tile6: daemon/ and the stb headers beside this script) still builds.
DAEMON=$HERE/daemon; [ -d "$DAEMON" ] || DAEMON=$HERE/../disclaunch
stbsrc() { [ -f "$HERE/$1" ] && echo "$HERE/$1" || echo "$HERE/../frontend/third_party/$1"; }
W=$HOME/rps5/tile6build; mkdir -p $W/obj && cd $W
for f in disctile.c tile.c tile.h; do sed 's/\r$//' "$HERE/$f" > $f; done
for f in stb_image.h stb_truetype.h; do sed 's/\r$//' "$(stbsrc $f)" > $f; done
for f in sgdb_art.cpp fe_https.cpp fe_https_ps5.cpp fe_https.h fe_https_roots.inc; do sed 's/\r$//' "$HERE/https/$f" > $f; done
CC=$SDK/bin/prospero-clang; CXX=$SDK/bin/prospero-clang++
printf '%s\n' "-DMBEDTLS_CONFIG_FILE=\\\"ps5sx2_mbedtls_config.h\\\"" "-I$MB/port" "-I$MB/include" "-I$MB/library" > mb.rsp
grep -o 'MBEDTLS_FILES += .*' $MB/port/files.mk | sed 's/MBEDTLS_FILES += //' > mbfiles.txt
if [ ! -f obj/mb_aes.c.o ]; then
  echo "compiling $(wc -l < mbfiles.txt) mbedTLS files ..."
  while read f; do echo "$f"; done < mbfiles.txt | xargs -P 8 -I{} sh -c "$CC -std=gnu11 -O2 -DNDEBUG @mb.rsp -include string.h -include stdlib.h -c $MB/library/{} -o obj/mb_{}.o 2>&1 | grep -v 'no version information' | head -5"
  $CC -std=gnu11 -O2 -DNDEBUG @mb.rsp -include string.h -include stdlib.h -c $MB/port/ps5sx2_mbedtls_platform.c -o obj/mb_platform.o 2>&1 | grep -v 'no version information' | head -5
fi
CXXF="-std=c++20 -O2 -DNDEBUG -fno-exceptions -fno-rtti @mb.rsp -I."
for f in sgdb_art fe_https fe_https_ps5; do $CXX $CXXF -c $f.cpp -o obj/$f.o 2>&1 | grep -v 'no version information' | head -20; done
$CC -std=gnu11 -O2 -w -c tile.c -o obj/tile.o 2>&1 | grep -v 'no version information' | head -20
$CC -std=gnu11 -O2 -Wall -Wextra -Werror -I$HOME/sqlite-ps5 -c disctile.c -o obj/disctile.o 2>&1 | grep -v 'no version information' | head -40
$CXX -o DiscTile.elf obj/*.o -L$HOME/sqlite-ps5 -lsqlite3 -L$SDK/target/lib -lSceNet -lpthread 2>&1 | grep -v 'no version information' | head -30
ls -la DiscTile.elf && cp DiscTile.elf "$HERE/DiscTile.elf"
