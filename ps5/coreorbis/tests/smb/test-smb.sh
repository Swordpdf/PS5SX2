#!/usr/bin/env bash
# PC test of OrbisSmb against Samba on loopback. Needs root and Samba (smbd, pdbedit, smbstatus), otherwise skipped.
# Creates a user "smbtest" if missing. SMB_SANITIZE=address,undefined, SMB_MAX_PROTOCOL=SMB2_02 (etc.) are optional.
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
coreorbis=$(cd -- "$here/../.." && pwd)
pcsx2=$(cd -- "$coreorbis/../.." && pwd)
smb="$pcsx2/ps5/third_party/libsmb2"
nfs="$pcsx2/ps5/third_party/libnfs"
CXX=${CXX:-clang++}
CC=${CC:-clang}

for tool in smbd pdbedit smbstatus; do
  if ! command -v "$tool" > /dev/null; then
    echo "SKIP: no $tool (Samba; see this script's header)"
    exit 0
  fi
done
if [[ $(id -u) != 0 ]]; then
  echo "SKIP: smbd wants root (see this script's header for a container)"
  exit 0
fi

work=${SMB_TEST_DIR:-$(mktemp -d /tmp/test-smb.XXXXXX)}
rm -rf "$work"
mkdir -p "$work/smb" "$work/served/public" "$work/served/private/PS2" "$work/samba"/{private,lock,state,cache,pid,ncalrpc}
chmod 755 "$work"
san=()
[[ -n ${SMB_SANITIZE:-} ]] && san=(-fsanitize="$SMB_SANITIZE" -fno-omit-frame-pointer)

smb_flags=(-O1 -g -w -DHAVE_CONFIG_H -D_FILE_OFFSET_BITS=64 "-D_U_=__attribute__((unused))" -I"$smb" -I"$smb/include"
  -I"$smb/include/smb2" "${san[@]}")
objs=()
for c in "$smb"/lib/*.c; do
  o="$work/smb/$(basename "$c" .c).o"
  "$CC" "${smb_flags[@]}" -c "$c" -o "$o" &
  objs+=("$o")
  while (( $(jobs -r | wc -l) >= ${SMB_JOBS:-4} )); do wait -n; done
done
mkdir -p "$work/nfs"
for c in "$nfs"/lib/*.c "$nfs"/mount/*.c "$nfs"/nfs/*.c "$nfs"/nfs4/*.c "$nfs"/portmap/*.c; do
  o="$work/nfs/$(basename "$(dirname "$c")")-$(basename "$c" .c).o"
  "$CC" -O1 -g -w -DHAVE_CONFIG_H -DHAVE_NFS4_2 "-D_U_=__attribute__((unused))" -I"$nfs" -I"$nfs/include" \
    -I"$nfs/include/nfsc" -I"$nfs/mount" -I"$nfs/nfs" -I"$nfs/nfs4" -I"$nfs/portmap" "${san[@]}" -c "$c" -o "$o" &
  objs+=("$o")
  while (( $(jobs -r | wc -l) >= ${SMB_JOBS:-4} )); do wait -n; done
done
wait
cxx_flags=(-std=c++20 -O1 -g -Wall -Wextra -Wno-unused-parameter -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
  -I"$coreorbis/include-orbis" -I"$smb/include" -I"$nfs/include" "${san[@]}")
wraps=()
while IFS= read -r name; do
  [[ -z $name || $name == \#* ]] && continue
  wraps+=("-Wl,--wrap=$name")
done < "$coreorbis/orbis-shims/OrbisNfs.wrap"
wraps+=(-Wl,--wrap=opendir -Wl,--wrap=readdir -Wl,--wrap=closedir)
"$CXX" "${cxx_flags[@]}" -DORBIS_NFS_WRAP_DIRS "$here/test_smb.cpp" "$coreorbis/orbis-shims/OrbisSmb.cpp" \
  "$coreorbis/orbis-shims/OrbisNfs.cpp" "${objs[@]}" "${wraps[@]}" -o "$work/test_smb" -lpthread

id smbtest > /dev/null 2>&1 || useradd -M -s /usr/sbin/nologin smbtest
chown -R smbtest "$work/served/private"
chmod -R a+rX "$work/served/public"
chmod 700 "$work/served/private"
port=$((24000 + RANDOM % 4000))
conf="$work/samba/smb.conf"
cat > "$conf" << EOF
[global]
  server role = standalone server
  smb ports = $port
  interfaces = 127.0.0.1
  bind interfaces only = yes
  private dir = $work/samba/private
  lock directory = $work/samba/lock
  state directory = $work/samba/state
  cache directory = $work/samba/cache
  pid directory = $work/samba/pid
  ncalrpc dir = $work/samba/ncalrpc
  log level = 1
  passdb backend = tdbsam
  map to guest = Bad User
  server min protocol = SMB2_02
  server max protocol = ${SMB_MAX_PROTOCOL:-SMB3}
  load printers = no
  printcap name = /dev/null
  disable spoolss = yes
[public]
  path = $work/served/public
  guest ok = yes
  read only = yes
[private]
  path = $work/served/private
  valid users = smbtest
  read only = yes
EOF
printf 'secret pass\nsecret pass\n' | pdbedit -s "$conf" -a -u smbtest -t > /dev/null
smbd -s "$conf" --foreground --no-process-group --debug-stdout > "$work/smbd.log" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null || true' EXIT
up=0
for _ in $(seq 1 100); do
  (exec 3<> "/dev/tcp/127.0.0.1/$port") 2> /dev/null && up=1 && break
  sleep 0.1
done
if (( !up )); then
  echo "FAIL: smbd isn't listening on 127.0.0.1:$port; its log:"
  cat "$work/smbd.log"
  exit 1
fi

export SMB_KILL_CLIENTS="smbstatus -s '$conf' -p 2>/dev/null | awk '\$1 ~ /^[0-9]+\$/ {print \$1}' | xargs -r kill -9"
"$work/test_smb" "$work/served/public" "$work/served/private" "$port" "$work/smb-passwords.txt"
