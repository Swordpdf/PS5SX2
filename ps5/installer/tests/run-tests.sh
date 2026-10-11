#!/bin/bash
# Host tests: make host, then tests/test_installer.py with the real zip and the console's listing.
set -euo pipefail
cd "$(dirname "$0")/.."
: "${PS5SX2_TEST_REAL_ZIP:=/mnt/user-data/uploads/temp/PS5SX2-TestBuild1-vk-285-112/PS5SX2-TestBuild1-vk-285-112.zip}"
: "${PS5SX2_TEST_LSALL:=/mnt/user-data/uploads/temp/live/lsall.txt}"
export PS5SX2_TEST_REAL_ZIP PS5SX2_TEST_LSALL
mkdir -p build && cc -shared -fPIC -O1 -o build/shim.so tests/shim.c -ldl
exec python3 tests/test_installer.py
