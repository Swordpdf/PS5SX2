# PS5SX2: the PS5 layer

PS5SX2 runs PCSX2 on a jailbroken PS5. Its renderer is PCSX2's Vulkan backend on a modified version of ps5vk, the Vulkan driver from [Mihawk-99](https://github.com/mihawk-99)'s [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan), tweaked specifically for PS5SX2 and linked into the app. It is developed on a PS5 Pro with firmware 11.40. The console runs the PS5SX2 Helper payload, which jailbreaks the app and mounts /data into its sandbox.

## Layout

- **The repository root** is PCSX2: upstream commit 450a39a, imported without its history, with the port's changes as commits on top.
- **`ps5/coreorbis/`**:
  - the app's entry point, `main-boot.cpp`;
  - the PS5 shims PCSX2 runs on, in `orbis-shims/` and `include-orbis/`;
  - the build, `Makefile.vk` and `link-vk.sh`.
- **`ps5/frontend/`**: the game shelf and the settings web page. `fe_shaders.inc` is generated from `shaders/` by `build_shaders.sh`, which needs glslangValidator.
- **`ps5/proto/native/`**: the host tool that converts and signs the eboot, plus the app's CRT and linker script. They come from BlackBearReloaded's ps5-native-app-boilerplate (GPL-3.0-or-later).
- **`ps5/tools/`**:
  - `build-mesa-util.sh`, from Mihawk-99's PS5_vkQuake;
  - `prepare_console_elf.py`.
- **`ps5/sce_sys/param.json`**: the app's parameters (title ID PPSA99203). The icon, the backgrounds and the music ship with the releases.

## Dependencies

- **The driver:** Swordpdf/PS5HB_Vulkan at commit 5da7401, built with its own scripts (`tools/setup-native-dependencies.sh`, then `tools/build-driver.sh`). `PS5_VULKAN_DIR` names the checkout. The line `sources: driver ...` at the top of a build's `boot.log` (and in its eboot's strings) names the driver commit the build was really linked with; a release's `NOTICE.txt` has to name the same one.
  - **The 120 Hz output:** frame generation shows a 60 fps game at 120 Hz and a PAL game at 50 fps at 100 only when the console's output is 119.88 Hz. That needs the output code of Mihawk-99's PS5_Vulkan (commits 94598bb, a17abd5, 8577153, 49679c9 and 060c5ac: the mode with `sceVideoOutIsOutputSupported` and `sceVideoOutConfigureOutput`, the measured vblank period, the restore before the handle closes) plus what PS5SX2 adds to it: the exports `ps5vk_output_refresh_millihertz`, `ps5vk_output_restore_default` and `ps5vk_output_status`, the title's `param.json` read from `PS5VK_PARAM_JSON`, and a 5 s wait before VideoOut is opened again after a 120 Hz output closed. The 2.02 build's driver (commit 964919b914) had all of it. The driver of commit 5da7401 has none of it, and the builds linked with it show a 60 Hz display (`[display] modes offered: 3840x2160 at 60.00 Hz`) and generate frames only for games at 30 fps or less (25 for PAL). Until a driver with the output code is linked, `orbis-shims/OrbisVideoOutMode.*` can do the switch from the app, opt-in (flag `fg120`; `nofg120` turns it off); it has not been tried on a console. Build 175 (2.11) is linked with the 964919b914 release again (its `boot.log` says `sources: driver 964919b914`), whose driver has the output code, so the app's own switch stays out of the way there.
  - The console only takes the 120 Hz output when the title's `param.json` declares high frame rates (`attribute3` 0x80040, in `sce_sys/param.json`) and the PS5's Settings > Screen and Video > Video Output has the 120 Hz output on with a resolution of Automatic or 4K; at 1080p it refuses it. A 50 Hz or 100 Hz output cannot be selected from a title (the console's output selectors 0 to 63 were tried: only 59.94 and 119.88 Hz answer).
- **`PS5SX2_DEPS`:** a folder holding:
  - `sdk285/`: the ps5-payload-dev SDK v0.42, with the changes to `target/lib` described below;
  - `ryml/`: rapidyaml at 0cd25a1, with its c4core submodule;
  - `glslang/`: glslang at 275822a. `glslang/build-ps5/` holds its static libraries, built with the SDK's CMake toolchain: Release, with `ENABLE_OPT`, `ENABLE_HLSL`, the binaries and the tests off;
  - `shaderc/`: shaderc v2026.2, used for its headers only.
- **Host tools:** LLVM 18 (`clang-18`, `clang++-18`, `ld.lld-18`, `llvm-objcopy-18`, `llvm-nm-18`), zlib's development files and Python 3.

Each path can also be set on its own: `RYML`, `PS5_PAYLOAD_SDK`, `GLSLANG`, `GLSLANG_BUILD` and `SHADERC_INCLUDE`.

**Not written down yet:** the SDK's `target/lib` differs from the stock v0.42 release.
- `crt1.o`, `libc++abi.a` and `libunwind.a` are the port's rebuilt copies.
- It adds the AGC import stubs `libSceAgc` and `libSceAgcDriver`, as `.so` and `.prx`.

The steps to recreate that folder from the stock SDK are still to be written down.

Public dependency preparation and the remaining release-build blockers are
recorded in [Preparing the PS5 build dependencies](docs/build-dependencies.md)
(AI-assisted).

## Build

The initial softcore RetroAchievements integration, its account screen, game achievement browser and
local checks are described in [RetroAchievements integration](docs/retroachievements.md)
(AI-assisted). The shelf and in-game web browsers were confirmed on the console;
the localized shelf credential lookup fix still requires console validation.

```sh
export PS5SX2_DEPS=/path/to/deps
export PS5_VULKAN_DIR=/path/to/PS5HB_Vulkan
cd ps5/coreorbis
make -f Makefile.vk -j"$(nproc)"
ORBIS_BUILD_TAG=my-build bash link-vk.sh
```

In a container instead, with the public dependencies above at their revisions (the stock SDK, without the `target/lib`
changes): `ps5/docker/build.sh` compiles; with `PS5_VULKAN_DIR` set, it links too.
`ps5/docker/build-radv.sh` compiles and links with the public RADV driver instead (ps5/docs/build-dependencies.md):
`ps5/build-radv/app/eboot.bin`, experimental.

- The first link also builds the host tool, into `ps5/proto/native/build/host/`.
- The signed eboot is `ps5/build-vk/app/eboot.bin`.
- On the console, the app folder `/data/homebrew/PPSA99203/` holds:
  - `eboot.bin`;
  - `sce_sys/`;
  - `sce_module/libc.prx`, which the driver repository generates with `make libc` (see its `runtime/README.md`).

## Licence and credits

PS5SX2 is GPL-3.0-or-later, like PCSX2; the licence text is `COPYING.GPLv3` at the root.

Third-party parts keep their licences:
- the stb headers: public domain or MIT;
- Project Nayuki's QR Code generator: MIT;
- the Font Awesome Free brands font and icons: SIL OFL 1.1 for the font, CC BY 4.0 for the icons (`frontend/assets/SOURCES.txt`, `frontend/assets/fonts/FontAwesome-LICENSE.txt`);
- OpenPS2BIOS 0.5 by Tamariba Devs, the open-source BIOS PS5SX2 ships for players without their own (embedded in the app, written to `cache/openps2bios/` with its licence when a player chooses it): MIT (`frontend/assets/openps2bios/LICENSE`, `frontend/assets/SOURCES.txt`).

The Discord and X logos are their owners' trademarks, used only to link to the author's accounts.

Thanks to:
- the PCSX2 Dev Team;
- [Mihawk-99](https://github.com/mihawk-99), for the Vulkan driver PS5SX2 runs on, in a version tweaked for it ([PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan)), and the RADV port ([PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa));
- BlackBearReloaded (ps5-native-app-boilerplate);
- John Törnblom (the ps5-payload-dev SDK);
- xlenore/ps2-covers and the [LaunchBox Games Database](https://gamesdb.launchbox-app.com), the covers the app downloads (not included here; `frontend/assets/launchbox-covers.txt` only lists which LaunchBox picture belongs to which serial, made by `tools/make_launchbox_covers.py`).
