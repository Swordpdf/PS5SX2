#!/bin/bash
# Fetches the installer's two libraries into $DEPS (default /home/claude/ps5deps/installer) and checks them.
#   Mbed TLS 3.6.7 (Apache-2.0): SHA-256 as in FreeBSD's ports (security/mbedtls3 distinfo)
#   miniz 3.1.1 (MIT): matches the copy OnionHEN vendors
set -euo pipefail
DEPS=${DEPS:-/home/claude/ps5deps/installer}
mkdir -p "$DEPS" && cd "$DEPS"
get() {
  local url=$1 file=$2 sum=$3
  [ -f "$file" ] || curl -fsSL --retry 3 -o "$file" "$url"
  echo "$sum  $file" | sha256sum -c -
}
get https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2 mbedtls-3.6.7.tar.bz2 \
  a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
get https://github.com/richgel999/miniz/releases/download/3.1.1/miniz-3.1.1.zip miniz-3.1.1.zip \
  cb28402bb2af93bdc331b60d16807e89727d1712a2d0a7ba0cac79a3e406fe40
[ -d mbedtls-3.6.7 ] || tar xjf mbedtls-3.6.7.tar.bz2
[ -d miniz-3.1.1 ] || (mkdir miniz-3.1.1 && cd miniz-3.1.1 && unzip -q ../miniz-3.1.1.zip)
echo "deps ready in $DEPS"
