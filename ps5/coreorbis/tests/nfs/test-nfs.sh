#!/usr/bin/env bash
# PS5 port: games on NFS shares (orbis-shims/OrbisNfs.cpp, ps5/third_party/libnfs) on a PC, against two go-nfs servers.
# vk-285-135 (AI-assisted).
#
#   OSNFS=/path/to/osnfs OSNFS_STRICT=/path/to/osnfsstrict ps5/coreorbis/tests/nfs/test-nfs.sh
#
# The servers: go-nfs's example/osnfs (github.com/willscott/go-nfs, Apache-2.0), and osnfsstrict.go beside this script
# (the same, refusing to mount any path but one; copy it to example/osnfsstrict/main.go in a go-nfs checkout and
# `go build -o osnfsstrict ./example/osnfsstrict`). Without them the test is skipped (exit 0, says so).
# NFS_SANITIZE=address,undefined builds with the sanitizers.
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
coreorbis=$(cd -- "$here/../.." && pwd)
pcsx2=$(cd -- "$coreorbis/../.." && pwd)
fe="$pcsx2/ps5/frontend"
nfs="$pcsx2/ps5/third_party/libnfs"
work=${NFS_TEST_DIR:-$here/obj}
CXX=${CXX:-clang++-18}
CC=${CC:-clang-18}

if [[ ! -x ${OSNFS:-} || ! -x ${OSNFS_STRICT:-} ]]; then
  echo "SKIP: set OSNFS and OSNFS_STRICT to the go-nfs test servers (see this script's header)"
  exit 0
fi

rm -rf "$work"
mkdir -p "$work/nfs" "$work/served/plain" "$work/served/strict"
san=()
[[ -n ${NFS_SANITIZE:-} ]] && san=(-fsanitize="$NFS_SANITIZE" -fno-omit-frame-pointer)

# libnfs as the console builds it (Makefile.vk's NFS_CSRCS), with the Linux config.
nfs_flags=(-O1 -g -w -DHAVE_CONFIG_H -DHAVE_NFS4_2 "-D_U_=__attribute__((unused))" -I"$nfs" -I"$nfs/include" -I"$nfs/include/nfsc"
  -I"$nfs/mount" -I"$nfs/nfs" -I"$nfs/nfs4" -I"$nfs/portmap" "${san[@]}")
objs=()
for c in "$nfs"/lib/*.c "$nfs"/mount/*.c "$nfs"/nfs/*.c "$nfs"/nfs4/*.c "$nfs"/portmap/*.c; do
  o="$work/nfs/$(basename "$(dirname "$c")")-$(basename "$c" .c).o"
  "$CC" "${nfs_flags[@]}" -c "$c" -o "$o" &
  objs+=("$o")
  while (( $(jobs -r | wc -l) >= ${NFS_JOBS:-2} )); do wait -n; done
done
wait

# The frontend's game scan (fe_games.cpp and what it needs), as the frontend's raw .bin test builds it.
"$CC" -O1 -c "$pcsx2/ps5/third_party/lz4/lz4.c" -o "$work/lz4.o"
"$CC" -O1 -I"$pcsx2/3rdparty/libchdr/include" -c "$fe/host/chd_stub.c" -o "$work/chd_stub.o"
cxx_flags=(-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
  -I"$coreorbis/include-orbis" -I"$nfs/include" -I"$fe" -I"$pcsx2/3rdparty/vulkan/include" -I"$pcsx2/3rdparty/libchdr/include"
  -I"$pcsx2/ps5/third_party/lz4" "${san[@]}")
"$CXX" "${cxx_flags[@]}" -DORBIS_NFS_WRAP_DIRS -c "$coreorbis/orbis-shims/OrbisNfs.cpp" -o "$work/OrbisNfs.o"

# The console's --wrap list, and the PC's opendir/readdir/closedir (the console's are dirshim.cpp's).
wraps=()
while IFS= read -r name; do
  [[ -z $name || $name == \#* ]] && continue
  wraps+=("-Wl,--wrap=$name")
done < "$coreorbis/orbis-shims/OrbisNfs.wrap"
wraps+=(-Wl,--wrap=opendir -Wl,--wrap=readdir -Wl,--wrap=closedir)
"$CXX" "${cxx_flags[@]}" "$here/test_nfs.cpp" "$work/OrbisNfs.o" "$fe/fe_games.cpp" "$fe/fe_covers.cpp" "$fe/fe_text.cpp" \
  "$fe/fe_i18n.cpp" "$fe/fe_settings.cpp" "$work/lz4.o" "$work/chd_stub.o" "${objs[@]}" "${wraps[@]}" \
  -o "$work/test_nfs" -lz -lpthread

# Two servers on loopback: 127.0.0.1 mounts any path; 127.0.0.2 only /export (its folder is the export's root).
port=$((21000 + RANDOM % 4000))
sport=$((port + 1))
"$OSNFS" "$work/served/plain" "$port" > "$work/osnfs.log" 2>&1 &
p1=$!
"$OSNFS_STRICT" "$work/served/strict" "127.0.0.2:$sport" /export > "$work/osnfsstrict.log" 2>&1 &
p2=$!
trap 'kill $p1 $p2 2>/dev/null || true' EXIT
for _ in $(seq 1 50); do
  grep -q "running" "$work/osnfs.log" 2>/dev/null && grep -q "running" "$work/osnfsstrict.log" 2>/dev/null && break
  sleep 0.1
done
"$work/test_nfs" "$work/served/plain" "$port" "$work/served/strict" "$sport"
