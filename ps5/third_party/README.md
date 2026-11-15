# Third-party code vendored for the PS5 port

The PS5 payload SDK has no zlib or zstd, and PCSX2 takes both from its dependency build rather than
`3rdparty/`. The port needs them for CHD images (vk-285-108): `3rdparty/libchdr` decompresses hunks with
zlib (the `zlib` and `cdzl` codecs), zstd (`zstd`, `cdzs`), LZMA (`lzma`, `cdlz`; from `3rdparty/lzma`),
Huffman and FLAC (in libchdr). CSO and ZSO images (vk-285-113) need zlib's inflate for a CSO's blocks and
LZ4 for a ZSO's. Only the decompression sources are here (LZ4's single `lz4.c` holds both directions), byte-identical
to the releases:

| Folder | Release | Tarball SHA-256 | License |
|---|---|---|---|
| `zlib/` | zlib 1.3.1 (`zlib-1.3.1.tar.gz`, zlib.net) | `9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23` | zlib (`zlib/LICENSE`) |
| `zstd/` | zstd 1.5.6 (`zstd-1.5.6.tar.gz`, github.com/facebook/zstd releases) | `8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1` | BSD (`zstd/LICENSE`), used under BSD; also GPLv2 (`zstd/COPYING`) |
| `lz4/` | LZ4 1.10.0 (`lz4-1.10.0.tar.gz`, github.com/lz4/lz4 releases) | `537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b` | BSD 2-Clause (`lz4/LICENSE`, the library's) |
| `libarchive/` | libarchive 3.8.9 (`libarchive-3.8.9.tar.xz`, libarchive.org/downloads) | `888c934f9d95648ecb9163dc8e23ab80a476ecb81a8f1154704a227b5b676dde` | BSD 2-Clause (`libarchive/COPYING`); the BLAKE2 files are CC0/OpenSSL/Apache 2.0, used under CC0 |
| `mbedtls/` | Mbed TLS 3.6.7 (`mbedtls-3.6.7.tar.bz2`, github.com/Mbed-TLS/mbedtls releases) | `a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6` | Apache-2.0 OR GPL-2.0-or-later (`mbedtls/LICENSE`), used under GPL-2.0-or-later |
| `libnfs/` | libnfs 18.0.0, git `7613236464317bee2d9636f087f995148de4db27` (github.com/sahlberg/libnfs) | (a git commit, no tarball) | LGPL-2.1-or-later (`libnfs/LICENCE-LGPL-2.1.txt`; `COPYING` says which files), used under GPL-3.0; the XDR files are BSD (`LICENCE-BSD.txt`) |
| `libsmb2/` | libsmb2 6.2, git tag `libsmb2-6.2`, `d67e213a5c4e7e4969fd81f0b95e4ca5831fbba1` (github.com/sahlberg/libsmb2) | (a git commit, no tarball) | LGPL-2.1-or-later (`libsmb2/LICENCE-LGPL-2.1.txt`) for most files, others below; used under GPL-3.0 |

- **zlib:** inflate only: `adler32.c crc32.c inffast.c inflate.c inftrees.c zutil.c` and their headers
  (`gzguts.h` because `zutil.c` includes it). No deflate, no gz* file functions.
- **zstd:** `lib/zstd.h`, `lib/zstd_errors.h`, `lib/common/` without `pool.*` and `threading.*`, and
  `lib/decompress/` without `huf_decompress_amd64.S` (built with `ZSTD_DISABLE_ASM`). No compression,
  dictionary builder or legacy formats.

- **lz4:** `lib/lz4.c`, `lib/lz4.h` and `lib/LICENSE` of the release (the library is BSD 2-Clause; the release's
  programs, GPLv2, are not here).

- **libarchive** (2026-10-05, AI-assisted): the texture pack downloads (`ps5/frontend/fe_texpacks.cpp`) unpack the
  packs of archive.org's "PCSX2 HD Texture Packs", which are RAR 5 files. Only the reading side, with the RAR (4 and 5)
  and zip readers: the 30 `.c` files in `libarchive/libarchive/` and the 25 headers they include, byte-identical to the
  release. `config.h` is ours: it picks `config_ps5.h`, what the release's own CMake checks found with the payload
  SDK's toolchain (`prospero-cmake`, every optional library off), plus zlib switched on at its end (the inflate
  above), or `config_linux.h`, the same build on Linux for the PC preview (`ps5/frontend/host`). The official unrar
  code is not used: its licence doesn't allow combining it with GPL code. Needs proper testing on more packs.
  libarchive has NO 7z reader here (no liblzma): the 192 `.7z` packs of the community's Google Drive folder are read by
  `ps5/frontend/fe_sevenzip.c` (b176, AI-assisted) with the LZMA SDK that is already in `3rdparty/lzma` (26.02, public
  domain; nothing new is vendored): `Makefile.vk` builds 13 more of its files (`SZ7_CSRCS`: 7zArcIn, 7zBuf, 7zCrc, 7zCrcOpt,
  7zDec, 7zStream, Bcj2, Bra, Bra86, BraIA64, CpuArch, Delta, Lzma2Dec; LzmaDec.c is in `CHD_CSRCS`). It decodes one solid block
  at a time as a stream (a refresh of the SDK needs `ps5/frontend/host/test-sevenzip.sh` again: the corpus, the fuzz, the memory bounds).

- **mbedtls** (2026-10-05, AI-assisted): the frontend's own HTTPS (`ps5/frontend/fe_https.cpp`), which the texture
  pack downloads and RetroAchievements (`ps5/coreorbis/orbis-shims/ProsperoHTTPConsole.cpp`) use. After the jailbreak the console's libSceSsl can't reach its certificate store, and with roots
  handed to it, it still can't follow cross-signed chains like archive.org's (diagnostic payloads texnet3-5). Only what
  `mbedtls/port/ps5sx2_mbedtls_config.h` compiles (a TLS 1.2 client with legacy crypto: ECDHE, AES-GCM and
  ChaCha20-Poly1305, RSA and ECDSA, X.509): the 41 `library/*.c` files listed in `mbedtls/port/files.mk`, and the
  `library/` and `include/` headers they include, byte-identical to the release. `port/` is ours: the config, and
  `ps5sx2_mbedtls_platform.c` (gmtime_r and the zeroing, which the PS5's libc lacks). The roots it trusts are
  Mozilla's, `ps5/frontend/fe_https_roots.inc`, made from certifi 2026.04.22's `cacert.pem` (MPL-2.0; SHA-256
  `16be3f6feb15408195dcfe3aa1a75ef9db72f646b96ebbefdc68f56255f799f8`, the same as that tag on GitHub) by
  `ps5/frontend/host/make-https-roots.py`. Tests: `ps5/frontend/host/test-https.sh`.

- **libnfs** (vk-285-135, AI-assisted): games on NFS shares (`ps5/coreorbis/orbis-shims/OrbisNfs.cpp`); the console's
  kernel has no NFS client. The library and the NFS v3, v4, MOUNT and portmapper protocol code: `lib/` (without the Windows
  `.def`), `include/`, `mount/`, `nfs/`, `nfs4/` and `portmap/` (the `.c`, `.h` and `.x` files), byte-identical to the
  commit but for one change in `lib/socket.c` (`create_socket`, marked PS5SX2: `SO_NOSIGPIPE` on the console's sockets).
  `config.h` is ours: `config_ps5.h` is what libnfs's CMake checks found with `prospero-cmake` (multithreading, tests,
  utilities and examples off), changed by hand where its header says (clock_gettime on, getpwuid and the process-wide
  SIGPIPE handler off); `config_linux.h` is the same build on Linux, for `ps5/coreorbis/tests/nfs/test-nfs.sh`. No
  Kerberos, no TLS, no NLM/NSM/rquota.

- **libsmb2**: SMB shares (`ps5/coreorbis/orbis-shims/OrbisSmb.cpp`). `lib/` (the `.c` files of its CMake build, so
  without `krb5-wrapper.c`, and the `.h` files), `include/smb2/`, and `include/`'s `libsmb2-private.h`,
  `portable-endian.h`, `slist.h` and `asprintf.h`, byte-identical to the commit except `lib/socket.c` (`SO_NOSIGPIPE`,
  marked in the file). `config.h` is ours; `config_ps5.h` and `config_linux.h` are libsmb2's CMake output with
  `prospero-cmake` and on Linux. Files under other terms than the LGPL: `lib/aes.*`, `lib/md5.*` and
  `include/portable-endian.h` are public domain; `lib/md4c.c` and `lib/md4.h` are derived from the RSA Data Security,
  Inc. MD4 Message-Digest Algorithm (RFC 1320; RSA's notice is in the files); `lib/sha*`, `lib/usha.c` and `lib/hmac.c`
  are the RFC 4634 code and `lib/hmac-md5.*` the RFC 2104 code, without notices.

`ps5/coreorbis/Makefile.vk` builds them (`CHD_CSRCS`, `LZ4_CSRCS`, `LIBARCHIVE_CSRCS`, `MBEDTLS_CSRCS`, `NFS_CSRCS`, `SMB_CSRCS`, and `SZ7_CSRCS` from `3rdparty/lzma`).
