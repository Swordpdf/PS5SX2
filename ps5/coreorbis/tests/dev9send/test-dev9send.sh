#!/usr/bin/env bash
# PS5 port: DEV9's TCP send that never waits long on the emulation's thread (include-orbis/OrbisDev9Send.h) and the ticker's stall line
# (include-orbis/OrbisStall.h), on a socket pair. killzone-online, 2026-10-30 (AI-assisted).
#   ps5/coreorbis/tests/dev9send/test-dev9send.sh
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../../../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
CXX=${CXX:-clang++-18}
"$CXX" -std=c++20 -O1 -g -Wall -Wextra -Werror -I"$root/ps5/coreorbis/include-orbis" "$here/dev9send_test.cpp" -o "$work/dev9send_test"
"$work/dev9send_test"
