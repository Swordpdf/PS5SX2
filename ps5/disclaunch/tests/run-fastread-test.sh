#!/usr/bin/env bash
# Host test of daemon/disc_fastread.h (helper 171h): ASAN+UBSAN, then -O2 with the helper's warnings. Run from anywhere (Linux / WSL, gcc or cc).
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
inc=${DFR_INC:-$here/..}
out=$(mktemp -d)
cc -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wextra -Werror -I"$inc" "$here/fastread_test.c" -o "$out/t_asan"
"$out/t_asan"
cc -std=gnu11 -O2 -Wall -Wextra -Werror -I"$inc" "$here/fastread_test.c" -o "$out/t_o2"
"$out/t_o2"
rm -rf "$out"
