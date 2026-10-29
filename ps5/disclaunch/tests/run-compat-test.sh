#!/usr/bin/env bash
# Host test of disc_compat.h + disc_compat_db.h (helper 171k: next to the PS5SX3 helper): ASAN+UBSAN, then -O2 with the helper's warnings.
# Needs sqlite3.h (SQLITE_INC, default ~/sqlite-ps5: the header the helper builds with) and a host libsqlite3 (SQLITE_LIB, default the system's .so.0).
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
inc=${DFR_INC:-$here/..}
sqinc=${SQLITE_INC:-$HOME/sqlite-ps5}
sqlib=${SQLITE_LIB:-/usr/lib/x86_64-linux-gnu/libsqlite3.so.0}
out=$(mktemp -d)
cc -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -Werror -I"$inc" -I"$here" -I"$sqinc" "$here/compat_test.c" "$sqlib" -o "$out/t_asan"
"$out/t_asan"
cc -std=gnu11 -O2 -Wall -Wextra -Werror -I"$inc" -I"$here" -I"$sqinc" "$here/compat_test.c" "$sqlib" -o "$out/t_o2"
"$out/t_o2" | tail -1
rm -rf "$out"
