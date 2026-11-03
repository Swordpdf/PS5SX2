#!/usr/bin/env bash
# Helper 171p (build 187): the stale-sandbox clean-up as it ships (make_heal_test.py cuts it out of disctile.c), built for the no-mount helper
# (DISC_SMP_OWNS_DATA=1) and for the one with its own mount thread (0); then the game tiles' copy of the rules, checked in gametile.c's text.
# ASAN+UBSAN, then -O2. HT_VERBOSE=1 prints every check and line.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
out=$(mktemp -d)
python3 "$here/make_heal_test.py" "$here/../disctile.c" "$out/heal_test.c"
flags=(-std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -Wno-format-truncation -pthread)
for owns in 1 0; do
  cc "${flags[@]}" -DDISC_SMP_OWNS_DATA=$owns -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all "$out/heal_test.c" -o "$out/t_asan_$owns"
  "$out/t_asan_$owns"
  cc "${flags[@]}" -DDISC_SMP_OWNS_DATA=$owns -O2 "$out/heal_test.c" -o "$out/t_o2_$owns"
  "$out/t_o2_$owns" | tail -1
done
gt="$here/../gametile.c"
grep -q "gt_heal_game_sandboxes" "$gt" && { echo "FAIL: gametile.c still cleans up at an uninstall's 409 (gt_heal_game_sandboxes)"; exit 1; }
grep -A4 "^static int gt_heal_one(" "$gt" | grep -q "if (heal_left_alone(name)) return 0;" ||
  { echo "FAIL: gt_heal_one does not leave what heal_left_alone names alone"; exit 1; }
grep -A24 "^static void gt_heal_tick(" "$gt" | grep -q "now - busy_since < 60" || { echo "FAIL: gt_heal_tick has no 60 s busy rule"; exit 1; }
grep -q "heal_sandboxes(2, \"an uninstall was refused as busy\"" "$here/../disctile.c" && { echo "FAIL: disctile.c still cleans up at an uninstall's 409"; exit 1; }
grep -q 'nomount).*OWNS="-DDISC_SMP_OWNS_DATA=1"' "$here/../build_helper_merged.sh" || { echo "FAIL: the no-mount build does not set DISC_SMP_OWNS_DATA=1"; exit 1; }
echo "game tiles and build: the 171p rules are in gametile.c and build_helper_merged.sh"
rm -rf "$out"
