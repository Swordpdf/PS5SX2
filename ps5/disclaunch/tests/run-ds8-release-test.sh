#!/usr/bin/env bash
# Helper 171l (build 184): DiscSpeed8's shell release (detach checked, SIGCONT, retries) and the fw-11.40-only gate, from the shipped disc_speed8.c.
# ASAN+UBSAN, then -O2. DS_VERBOSE=1 prints every check.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
out=$(mktemp -d)
python3 "$here/make_ds8_release_test.py" "$here/../disc_speed8.c" "$out/ds8_test.c"
flags=(-std=gnu11 -Wall -Wextra -Werror -Wno-unused-function)
cc "${flags[@]}" -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all "$out/ds8_test.c" -o "$out/t_asan"
"$out/t_asan"
cc "${flags[@]}" -O2 "$out/ds8_test.c" -o "$out/t_o2"
"$out/t_o2" | tail -1
rm -rf "$out"
