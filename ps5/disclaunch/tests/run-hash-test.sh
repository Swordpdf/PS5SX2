#!/usr/bin/env bash
# Host test: disc_hash.c against Python's zlib/hashlib on sizes around the 64-byte block and 56-byte padding edges.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
out=$(mktemp -d)
cc -O2 -Wall -Wextra -Werror -o "$out/hash_test" "$here/hash_test.c" "$here/../disc_hash.c"
fail=0
for size in 0 1 55 56 57 63 64 65 119 120 121 2352 37632 1000003; do
  for piece in 1 37 64 2352 1048576; do
    head -c "$size" /dev/urandom > "$out/in"
    got=$("$out/hash_test" "$piece" < "$out/in")
    want=$(python3 -I -c 'import sys,zlib,hashlib; d=open(sys.argv[1],"rb").read(); print("%08x %s %s" % (zlib.crc32(d)&0xffffffff, hashlib.md5(d).hexdigest(), hashlib.sha1(d).hexdigest()))' "$out/in")
    if [ "$got" != "$want" ]; then echo "FAIL size $size piece $piece: $got != $want"; fail=1; fi
  done
done
rm -rf "$out"
[ $fail = 0 ] && echo "PASS disc_hash: 70 cases match zlib/hashlib"
exit $fail
