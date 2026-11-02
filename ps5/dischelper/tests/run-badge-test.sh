#!/usr/bin/env bash
# Helper 171n (build 186): the PS2 badge on PS5SX2's tiles (make_badge_test.py cuts the shipped functions out of disctile.c). Built against the host's
# SQLite with the helper's sqlite3.h (SQLITE_H_DIR, default ~/sqlite-ps5). ASAN+UBSAN, then -O2. BT_VERBOSE=1 prints every check; BT_REAL_DIR=<folder
# with copies of a console's app.db and appinfo.db> also runs the pass on those copies.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
out=$(mktemp -d)
inc=${SQLITE_H_DIR:-$HOME/sqlite-ps5}
lib=$(ls /usr/lib/x86_64-linux-gnu/libsqlite3.so.0 2>/dev/null || true)
[ -n "$lib" ] || { echo "no host libsqlite3.so.0"; exit 1; }
python3 "$here/make_badge_test.py" "$here/../disctile.c" "$out/badge_test.c"
flags=(-std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -I"$inc")
cc "${flags[@]}" -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all "$out/badge_test.c" "$lib" -o "$out/t_asan"
"$out/t_asan"
cc "${flags[@]}" -O2 "$out/badge_test.c" "$lib" -o "$out/t_o2"
"$out/t_o2" | tail -1
rm -rf "$out"
