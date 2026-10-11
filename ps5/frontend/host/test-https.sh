#!/usr/bin/env bash
# PS5 port frontend: the tests of our HTTPS client (https_test.cpp) on a PC (2026-10-05, AI-assisted). Builds the
# vendored mbedTLS with the port's config, fe_https.cpp and the test with the sanitizers, runs the unit tests, then the
# local servers of https_testserver.py (needs Python's "cryptography"). HTTPS_REAL=1 also fetches from archive.org
# (through $https_proxy's CONNECT when it is set); HTTPS_STATIC_OUT=<file> instead writes a static, sanitizer-free
# binary for another Linux PC (for "https_test real" there).
#
#   ps5/frontend/host/test-https.sh
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
fe=$(cd -- "$here/.." && pwd)
mb=$(cd -- "$fe/../third_party/mbedtls" && pwd)
CXX=${CXX:-clang++-18}
CC=${CC:-clang-18}
work=$(mktemp -d "${TMPDIR:-/tmp}/https.XXXXXX")
server_pid=
cleanup() {
  [[ -n $server_pid ]] && kill "$server_pid" 2>/dev/null || true
  rm -rf "$work"
}
trap cleanup EXIT

if [[ -n ${HTTPS_STATIC_OUT:-} ]]; then
  flags=(-O2 -static)
else
  flags=(-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
fi
defs=(-DMBEDTLS_CONFIG_FILE='"ps5sx2_mbedtls_config.h"' -I"$mb/port" -I"$mb/include")

mkdir -p "$work/obj"
objs=()
for c in $(sed -n 's/^MBEDTLS_FILES += //p' "$mb/port/files.mk") "$mb/port/ps5sx2_mbedtls_platform.c"; do
  [[ $c = /* ]] || c="$mb/library/$c"
  o="$work/obj/$(basename "$c" .c).o"
  "$CC" "${flags[@]}" -std=c99 -Wall -Wextra -Werror "${defs[@]}" -I"$mb/library" -c "$c" -o "$o"
  objs+=("$o")
done
"$CXX" -std=c++20 "${flags[@]}" -Wall -Wextra -Werror "${defs[@]}" -I"$fe" \
  "$here/https_test.cpp" "$fe/fe_https.cpp" "${objs[@]}" -lpthread -o "$work/https_test"

if [[ -n ${HTTPS_STATIC_OUT:-} ]]; then
  cp "$work/https_test" "$HTTPS_STATIC_OUT"
  echo "wrote $HTTPS_STATIC_OUT"
  exit 0
fi

"$work/https_test" units

python3 "$here/https_testserver.py" "$work/pki" > "$work/servers.json" &
server_pid=$!
for _ in $(seq 1 100); do
  [[ -s $work/servers.json ]] && break
  sleep 0.2
done
[[ -s $work/servers.json ]] || { echo "the test servers didn't start" >&2; exit 1; }
"$work/https_test" local "$work/servers.json"

if [[ -n ${HTTPS_REAL:-} ]]; then
  "$work/https_test" real
fi
