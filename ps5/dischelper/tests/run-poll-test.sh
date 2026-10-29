#!/usr/bin/env bash
# Helper 171k: the drive polling of disctile.c as it ships, next to the PS5SX3 helper (make_poll_test.py cuts it out and puts a made-up world
# around it). ASAN+UBSAN, then -O2. Needs python3, sqlite3.h (SQLITE_INC, default ~/sqlite-ps5) and a host libsqlite3 (SQLITE_LIB).
# PT_VERBOSE=1 prints the payload's log lines and every check.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
dl=$here/../../disclaunch
sqinc=${SQLITE_INC:-$HOME/sqlite-ps5}
sqlib=${SQLITE_LIB:-/usr/lib/x86_64-linux-gnu/libsqlite3.so.0}
out=$(mktemp -d)
python3 "$here/make_poll_test.py" "$here/../disctile.c" "$out/poll_test.c"
flags=(-std=gnu11 -Wall -Wextra -Werror -Wno-format-truncation -Wno-unused-function -I"$dl" -I"$dl/tests" -I"$sqinc")
cc "${flags[@]}" -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all "$out/poll_test.c" "$sqlib" -o "$out/t_asan"
"$out/t_asan"
cc "${flags[@]}" -O2 "$out/poll_test.c" "$sqlib" -o "$out/t_o2"
"$out/t_o2" | tail -1
rm -rf "$out"
