#!/usr/bin/env bash
# Helper 171l (build 184): the Reload tile's removal by the person sticks (make_reload_test.py cuts the shipped block out of disctile.c).
# ASAN+UBSAN, then -O2. RT_VERBOSE=1 prints every check.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
out=$(mktemp -d)
python3 "$here/make_reload_test.py" "$here/../disctile.c" "$out/reload_test.c"
flags=(-std=gnu11 -Wall -Wextra -Werror -Wno-unused-function)
cc "${flags[@]}" -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all "$out/reload_test.c" -o "$out/t_asan"
"$out/t_asan"
cc "${flags[@]}" -O2 "$out/reload_test.c" -o "$out/t_o2"
"$out/t_o2" | tail -1
rm -rf "$out"
