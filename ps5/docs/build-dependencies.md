# Preparing the PS5 build dependencies (AI-assisted)

The release build requires the author's modified SDK and Vulkan driver. A stock
payload SDK plus the public upstream driver is not yet a verified replacement.
See [the port's dependency list](../README.md#dependencies).
An experimental executable was successfully built using the existing RADV
link path and public dependencies instead; see the RADV attempt below.

## Local preparation

From the repository root, use the existing dependency checkouts:

```sh
export PS5SX2_DEPS="$PWD/deps"
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk

cmake -S deps/glslang -B deps/glslang/build-ps5 \
  -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DENABLE_OPT=OFF -DENABLE_HLSL=OFF \
  -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF
cmake --build deps/glslang/build-ps5 -j2

make -C ps5/coreorbis -f Makefile.vk -j2
```

The rapidyaml checkout must be at `0cd25a1` with its submodules, glslang at
`275822a`, and shaderc at `v2026.2`. This configuration successfully compiled
glslang with the locally installed payload SDK and Clang 23.1.1. The original
port uses LLVM 18 for its link tools; successful compilation with Clang 23 does
not establish console compatibility.

The two new RetroAchievements platform sources (`ProsperoHTTPDownloader.cpp`
and `ProsperoAchievements.cpp`) also compiled successfully with this native
SDK. This checks compiler/sysroot compatibility, not console execution.
The full `Makefile.vk` compilation subsequently completed with
`VK COMPILE OK: 480 objects`. The original ps5vk release was not linked;
the separate RADV attempt below produced an experimental executable.

## AGC import metadata

The [DNNDHH symbol listings](https://github.com/DNNDHH/PS5-3.20_Libs) contain
`libSceAgc.c` and `libSceAgcDriver.c`. The local generator reads their global
symbol declarations without compiling or executing the C implementations:

```sh
# Make a separate SDK copy first, if deps/sdk285 does not already exist.
cp -a /opt/ps5-payload-sdk deps/sdk285
python3 ps5/tools/generate-agc-imports.py \
  deps/agc-symbols deps/sdk285/target/lib
export PS5_PAYLOAD_SDK="$PS5SX2_DEPS/sdk285"
```

Place the two downloaded symbol listings in `deps/agc-symbols` beforehand.
The generated `.so` files contain only linker metadata: export names and the
module SONAME. They provide no runtime implementation and must not be deployed
to the console. The native app builder reads `.so` metadata and normalizes
`.sprx` SONAMEs into `.prx` import names. This step does not reproduce the
author's rebuilt CRT, C++ ABI library or unwinder. Availability of the firmware
3.20 symbols on a target console remains unverified.

## Remaining driver dependency

`Swordpdf/PS5HB_Vulkan`, referenced by the port, returned `Repository not found`
on an anonymous Git request during this investigation. The PS5SX2 release asset
list did not expose a separate driver archive or modified SDK.

[mihawk-99/PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) is the public
upstream. It has the same driver archive names and build scripts, but the port
documents additional driver changes. Its current bootstrap also needs the
[PS5_PayloadSDK fork](https://github.com/mihawk-99/PS5_PayloadSDK), the OpenGL
shader compiler bundle and Mesa. The SDK fork explicitly retains the upstream
CRT/C++ runtime binaries, so it does not supply Swordpdf's rebuilt copies.
Its `crt/` directory and `libcxx.sh` provide the upstream CRT sources and the
LLVM 18.1.8 C++ runtime build recipe; rebuilding those unchanged still does not
reproduce the port's undocumented modifications.

To reproduce the release, obtain the driver's source at the revision documented
by the port, or its `ps5vk-*.zip` development bundle (four static archives and
three Mesa utility objects), plus the SDK runtime patches or rebuilt files.
Using the upstream driver instead requires a separate compatibility port and
console validation; setting `PS5_VULKAN_DIR` to its source directory alone
does not finish the build.

## Successful RADV build attempt (console validation pending)

The existing RADV path built and signed `ps5/build-radv/app/eboot.bin` without
changing the public driver or reconstructing Swordpdf's runtime patches.
Native conversion reported zero unresolved non-stub imports; the signed
container's integrity check passed. This does not establish runtime stability,
game compatibility or working RetroAchievements authentication on the console.

Inputs used:

- PS5_Vulkan: `3f3ee69`.
- PS5_Mesa: `0b2d6d1a61d9bbf89cf8beb88a696144f67c61f8` (the driver's pin).
- PS5_PayloadSDK platform: `95c08f27386fc698f6bbe21dde3030140a41d10b` (the driver's pin).
- Clang/LLD 23.1.1 and Meson 1.12.1, with the upstream SDK runtime in `deps/sdk285`.

The SDK bootstrap and RADV driver were built by their existing scripts:

```sh
PS5_PAYLOAD_SDK_FORK="$PWD/deps/PS5_PayloadSDK" BUILD_JOBS=2 \
  bash deps/PS5_Vulkan/tools/setup-native-dependencies.sh
MESON="$PWD/deps/build-env/bin/meson" \
  PS5_MESA_FORK="$PWD/deps/PS5_Mesa" \
  PATH="$PWD/deps/build-env/bin:$PATH" \
  bash deps/PS5_Vulkan/tools/build-radv.sh release
```

To repeat the executable link from the repository root with the prepared local
dependencies and the locally built native tool:

```sh
export PS5SX2_DEPS="$PWD/deps"
export PS5_PAYLOAD_SDK="$PS5SX2_DEPS/sdk285"
export PS5_RADV_ARCHIVE="$PS5SX2_DEPS/PS5_Vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
export PS5_RADV_PLATFORM="$PS5SX2_DEPS/PS5_Vulkan/.deps/native/ps5-payload-sdk/target/lib/libps5platform.a"
export PS5_RADV_BUILTINS=/usr/lib/clang/23/lib/linux/libclang_rt.builtins-x86_64.a
export PS5_RADV_REV=0b2d6d1a61d9
export PS5_NATIVE_TOOL="$PS5SX2_DEPS/native-tools/ps5-native-tool"
export LLD=ld.lld NM=llvm-nm HOST_CXX=clang++
export LLD_EXTRA='--no-dynamic-linker -z nodynamic-undefined-weak'

make -C ps5/coreorbis -f Makefile.vk -j2
ORBIS_BUILD_TAG=ra-radv-test-1 ORBIS_TEST_BUILD=1 \
  bash ps5/coreorbis/link-radv.sh
```

The first link attempt left the optional legacy-driver debug hooks and libc's
optional dynamic-loader hooks as weak imports, which the native converter
rejected. LLD 23's `-z nodynamic-undefined-weak` resolved absent weak hooks to
zero, without supplying fake implementations or changing driver code.

The generated file is an executable replacement for an existing PS5SX2
installation, not a complete app package: keep the installation's `sce_sys`,
assets, `sce_module/libc.prx` and Helper. Back up its current `eboot.bin` before
replacing `/data/homebrew/PPSA99203/eboot.bin`. Start with the shelf and account
screen, then test a game. For failures, collect `boot.log` and `stderr.log` from
the configured PCSX2 data directory; do not share `achievements-secrets.ini`.
