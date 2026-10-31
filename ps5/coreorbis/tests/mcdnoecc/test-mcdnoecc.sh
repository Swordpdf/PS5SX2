#!/usr/bin/env bash
# PS5 port (b185, AI-assisted): .mc2 / .bin memory cards (no ECC: MemCard PRO2, SD2PSX) on a PC - include-orbis/OrbisMcdNoEcc.h (names in
# any case, the ECC conversion both ways, the leftover rule, the durable write) and the frontend's card list (fe_settings.cpp
# HasCardExtension, cut out as it ships).
#   ps5/coreorbis/tests/mcdnoecc/test-mcdnoecc.sh
#
# SPDX-License-Identifier: GPL-3.0+
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
inc=$(cd -- "$here/../../include-orbis" && pwd)
fe=$(cd -- "$here/../../../frontend" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# fe_settings.cpp's Lower() and HasCardExtension() as they ship
python3 - "$fe/fe_settings.cpp" "$work/fe_cards_cut.h" <<'EOF'
import re, sys
src = open(sys.argv[1], encoding="utf-8").read()
def cut(sig):
    a = src.index(sig)
    b = src.index("\n}\n", a) + 3
    return src[a:b]
out = "#pragma once\n#include <algorithm>\n#include <cctype>\n#include <cstring>\n#include <string>\nnamespace fe {\n"
out += cut("std::string Lower(std::string s)") + cut("bool HasCardExtension(const std::string& name)") + "}\n"
open(sys.argv[2], "w", encoding="utf-8").write(out)
EOF
CXX=${CXX:-clang++-18}
"$CXX" -std=c++20 -O2 -Wall -Wextra -Werror -I"$inc" -I"$work" "$here/mcdnoecc_test.cpp" -o "$work/mcdnoecc_test"
"$work/mcdnoecc_test" "$work/cards"
