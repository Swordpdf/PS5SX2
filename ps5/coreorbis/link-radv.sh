#!/usr/bin/env bash
# PCSX2 PS5 port -- link and sign the Vulkan build on RADV (vk-285-115, experimental).
#
# The same objects as link-vk.sh (Makefile.vk's), linked with mihawk-99's port of Mesa's RADV instead of ps5vk:
# mihawk-99/PS5_Vulkan builds it from his Mesa fork (tools/build-radv.sh release) into one archive, RADV + ACO + NIR +
# Mesa's Vulkan runtime, and links titles with tools/radv-link.sh, whose recipe is followed here:
#   - the archive under --whole-archive (Mesa's dispatch tables name every entry point weakly);
#   - the payload SDK fork's platform layer (mihawk-99/PS5_PayloadSDK, libps5platform.a), bound to libc's names:
#     every allocation to its heap in direct memory (--wrap malloc...), thread stacks in direct memory (--wrap
#     pthread_create...), open_memstream's fflush/fclose, and the libc functions no system module gives a title
#     (--defsym name=ps5_name, kept local by a version script);
#   - Clang's builtins (__cpu_model for Mesa's CPU detection).
# What differs from the recipe, to keep this port's own behaviour where RADV isn't involved:
#   - the names only PCSX2 uses and the recipe would rebind (clock_nanosleep, gmtime_r, getaddrinfo, freeaddrinfo) stay as
#     link-vk.sh resolves them, and if_nameindex/if_freenameindex stay the port's (orbis-shims/ProsperoMisc.cpp);
#   - the directory functions are the platform's as a family (RADV uses dirfd and rewinddir on its DIRs), so the port's
#     own opendir/readdir/closedir (orbis-shims/dirshim.cpp) are left out of this link;
#   - the archive carries its own zlib (a Mesa subproject), so the port's inflate sources (ps5/third_party/zlib) are left out.
#   - games on NFS shares (vk-285-135): the C library's calls in orbis-shims/OrbisNfs.wrap go through OrbisNfs.cpp, as
#     link-vk.sh. fclose and fflush are wrapped by both it and the platform layer (open_memstream's), so the two are
#     chained: the platform's __wrap_<name> is renamed in a copy of libps5platform.a, and OrbisNfs.o's __real_<name>
#     (in a copy) calls it. A call goes OrbisNfs (NFS FILEs) -> the platform (memstreams) -> the C library.
# The eboot finds out which driver it has at run time (include-orbis/OrbisDriver.h); nothing is compiled differently.
#
#   make -f Makefile.vk -j2 && PS5_RADV_ARCHIVE=... PS5_RADV_PLATFORM=... bash link-radv.sh
#
# Inputs (PS5SX2_DEPS/radv/ by default):
#   PS5_RADV_ARCHIVE     libvulkan_radeon.ps5.a (PS5_Vulkan .deps/native/radv-release/lib/)
#   PS5_RADV_REV         its Mesa fork revision, for boot.log (default: from the PROVENANCE.txt beside it, else "unknown")
#   PS5_RADV_PLATFORM    libps5platform.a of the SDK fork revision the archive was built with (its PROVENANCE's "sdk:")
#   PS5_RADV_BUILTINS    libclang_rt.builtins-x86_64.a
# Output: $VK_OUT/app/eboot.bin (default ps5/build-radv), as link-vk.sh. Needs testing on the console.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
port=$(cd -- "$here/.." && pwd)
PCSX2=${PCSX2:-$(cd -- "$port/.." && pwd)}
if [[ -z ${RYML:-} || -z ${PS5_PAYLOAD_SDK:-} || -z ${GLSLANG_BUILD:-} ]]; then
  : "${PS5SX2_DEPS:?set PS5SX2_DEPS (see ps5/README.md), or RYML, PS5_PAYLOAD_SDK and GLSLANG_BUILD}"
fi
RYML=${RYML:-$PS5SX2_DEPS/ryml}
SDK=${PS5_PAYLOAD_SDK:-$PS5SX2_DEPS/sdk285}
NATIVE=${PS5_NATIVE:-$port/proto/native}
PREPARE=${PS5_PREPARE_ELF:-$port/tools/prepare_console_elf.py}
TOOL=${PS5_NATIVE_TOOL:-$NATIVE/build/host/ps5-native-tool}
GLSLANG=${GLSLANG_BUILD:-$PS5SX2_DEPS/glslang/build-ps5}
RADV_ARCHIVE=${PS5_RADV_ARCHIVE:-$PS5SX2_DEPS/radv/libvulkan_radeon.ps5.a}
RADV_PLATFORM=${PS5_RADV_PLATFORM:-$PS5SX2_DEPS/radv/libps5platform.a}
RADV_BUILTINS=${PS5_RADV_BUILTINS:-$PS5SX2_DEPS/radv/libclang_rt.builtins-x86_64.a}
OUT=${VK_OUT:-$port/build-radv}
TAG=${ORBIS_BUILD_TAG:-vk-285-1-radv}
TEST_BUILD=${ORBIS_TEST_BUILD:-0}
LLD=${LLD:-ld.lld-18}
NM=${NM:-llvm-nm-18}
OBJCOPY=${OBJCOPY:-$(command -v llvm-objcopy-18 || command -v llvm-objcopy)}
export PS5_PAYLOAD_SDK=$SDK
export PS5_CLANG=${PS5_CLANG:-$(command -v clang-18 || command -v clang)}

for f in "$RADV_ARCHIVE" "$RADV_PLATFORM" "$RADV_BUILTINS"; do
  [[ -f $f ]] || { echo "[link-radv] error: $f is missing" >&2; exit 1; }
done
radv_rev=${PS5_RADV_REV:-}
provenance="$(dirname -- "$RADV_ARCHIVE")/PROVENANCE.txt"
if [[ -z $radv_rev && -f $provenance ]]; then
  radv_rev=$(sed -n 's/^revision: \([0-9a-f]\{12\}\).*/\1/p' "$provenance" | head -n 1)
fi
DRIVER_REV="radv ${radv_rev:-unknown}"

src_rev() {
  local rev
  rev=$(git -C "$1" rev-parse --short=10 HEAD 2>/dev/null || echo unknown)
  if [[ -n $(git -C "$1" status --porcelain --untracked-files=no -- . 2>/dev/null) ]]; then
    rev="$rev+"
  fi
  echo "$rev"
}
PCSX2_REV=$(src_rev "$PCSX2")
echo "[link-radv] sources: driver $DRIVER_REV ($(sha256sum "$RADV_ARCHIVE" | cut -c1-12)), pcsx2 $PCSX2_REV"

mkdir -p "$OUT/build/obj" "$OUT/app/sce_sys" "$OUT/app/sce_module" "$OUT/app/assets"

# 1. main-boot.cpp and the CRT, exactly as link-vk.sh compiles them.
sh "$NATIVE/tooling/prospero-clang18" \
  -std=c++20 -O2 -march=znver2 -msse4.1 -mavx2 -mno-vzeroupper -D_M_X86=1 -D__POSIX__=1 \
  -DOVERRIDE_HOST_PAGE_SIZE=0x4000 -DOVERRIDE_HOST_CACHE_LINE_SIZE=64 \
  -DORBIS_VULKAN=1 -DPS5SX2_ACHIEVEMENTS=1 -DORBIS_BUILD_TAG="\"$TAG\"" -DORBIS_TEST_BUILD="$TEST_BUILD" \
  -DORBIS_DRIVER_REV="\"$DRIVER_REV\"" -DORBIS_PCSX2_REV="\"$PCSX2_REV\"" \
  -Wno-macro-redefined -Wno-unused-function -include common/Threading.h \
  -I"$here/include-orbis" -I"$PCSX2" -I"$PCSX2/pcsx2" \
  -I"$PCSX2/3rdparty/include" -I"$PCSX2/3rdparty/fmt/include" \
  -I"$PCSX2/3rdparty/fast_float/include" -I"$PCSX2/3rdparty/rapidjson/include" \
  -I"$PCSX2/3rdparty/simpleini/include" -I"$PCSX2/3rdparty/ccc/src" \
  -I"$PCSX2/3rdparty/xbyak" -I"$PCSX2/3rdparty/zydis/include" \
  -I"$PCSX2/3rdparty/zydis/dependencies/zycore/include" \
  -I"$PCSX2/3rdparty/discord-rpc/include" -I"$PCSX2/3rdparty/demangler/include" \
  -I"$PCSX2/3rdparty/rcheevos/include" -I"$PCSX2/3rdparty/imgui/include" \
  -I"$PCSX2/3rdparty/soundtouch/soundtouch" -I"$PCSX2/3rdparty/freesurround/include" \
  -I"$RYML/src" -I"$RYML/ext/c4core.src" \
  -ffunction-sections -fdata-sections \
  -c "$here/main-boot.cpp" -o "$OUT/build/obj/main-boot.o"
for item in app_crt app_cpp_runtime; do
  sh "$NATIVE/tooling/prospero-clang18" \
    -std=c++20 -O2 -Wall -Wextra \
    -ffunction-sections -fdata-sections \
    -c "$NATIVE/tooling/native/$item.cpp" -o "$OUT/build/obj/$item.o"
done
sh "$NATIVE/tooling/prospero-clang18" -c "$here/radv-bindings.S" -o "$OUT/build/obj/radv-bindings.o"
echo "[link-radv] main-boot ($TAG, test build $TEST_BUILD), crt and binding objects OK"

# 2. The port's objects (Makefile.vk's list, in its order), without the ones RADV's link replaces (above).
make -C "$here" -f Makefile.vk -s objects.txt PCSX2="$PCSX2" RYML="$RYML" PS5_PAYLOAD_SDK="$SDK"
OBJS="$OUT/build/obj/app_crt.o $OUT/build/obj/app_cpp_runtime.o $OUT/build/obj/main-boot.o $OUT/build/obj/radv-bindings.o"
left_out=()
NFS_OBJ=
while IFS= read -r o; do
  case $o in
    */ps5/third_party/zlib/*.o | */orbis-shims/dirshim.o) left_out+=("$o") ;;
    */orbis-shims/OrbisNfs.o) NFS_OBJ="$here/$o"; OBJS="$OBJS @NFS_OBJ@" ;; # its chained copy, made in 3.
    *) OBJS="$OBJS $here/$o" ;;
  esac
done < "$here/objects.txt"
echo "[link-radv] $(( $(wc -l < "$here/objects.txt") - ${#left_out[@]} )) port objects (left out for RADV: ${left_out[*]##*/})"
(( ${#left_out[@]} == 7 )) || { echo "[link-radv] error: expected 7 objects to leave out (6 zlib, dirshim), found ${#left_out[@]}" >&2; exit 1; }

GLSLANG_LIBS=(
  "$GLSLANG/glslang/libglslang.a"
  "$GLSLANG/glslang/libglslang-default-resource-limits.a"
  "$GLSLANG/glslang/OSDependent/Unix/libOSDependent.a"
)

# 3. The platform layer's bindings (PS5_Vulkan tools/radv-link.sh at d609d71, less the names noted at the top).
radv_flags=(--wrap=pthread_create --wrap=pthread_join --wrap=pthread_detach --wrap=fclose --wrap=fflush)
for name in malloc calloc realloc free posix_memalign aligned_alloc memalign \
    malloc_usable_size reallocf reallocarray getline getdelim; do
  radv_flags+=("--wrap=$name")
done
# vk-285-135 (AI-assisted): games on NFS shares, as link-vk.sh (orbis-shims/OrbisNfs.wrap). Where the platform layer
# already has a __wrap_<name> (fclose and fflush, open_memstream's), the two are chained rather than duplicated:
# the platform's becomes ps5sx2_platform_wrap_<name> in a copy of its archive, and OrbisNfs.o's __real_<name> in a
# copy of it calls that, so a call goes OrbisNfs -> the platform -> the C library.
[[ -n $NFS_OBJ && -f $NFS_OBJ ]] || { echo "[link-radv] error: orbis-shims/OrbisNfs.o is not in objects.txt" >&2; exit 1; }
[[ -n $OBJCOPY ]] || { echo "[link-radv] error: llvm-objcopy not found (set OBJCOPY)" >&2; exit 1; }
"$NM" --defined-only -j "$RADV_PLATFORM" 2>/dev/null | sed -n 's/^__wrap_//p' | sort -u > "$OUT/build/radv-platform-wraps.txt"
nfs_count=0
chained=()
platform_renames=()
nfs_renames=()
while IFS= read -r name; do
  [[ -z $name || $name == \#* ]] && continue
  nfs_count=$((nfs_count + 1))
  if grep -qxF -- "$name" "$OUT/build/radv-platform-wraps.txt"; then
    chained+=("$name")
    platform_renames+=(--redefine-sym "__wrap_$name=ps5sx2_platform_wrap_$name")
    nfs_renames+=(--redefine-sym "__real_$name=ps5sx2_platform_wrap_$name")
  fi
  [[ " ${radv_flags[*]} " == *" --wrap=$name "* ]] || radv_flags+=("--wrap=$name")
done < "$here/orbis-shims/OrbisNfs.wrap"
PLATFORM_LIB="$OUT/build/libps5platform-chained.a"
NFS_LINKED="$OUT/build/obj/OrbisNfs-chained.o"
"$OBJCOPY" "${platform_renames[@]}" "$RADV_PLATFORM" "$PLATFORM_LIB"
"$OBJCOPY" "${nfs_renames[@]}" "$NFS_OBJ" "$NFS_LINKED"
OBJS=${OBJS/@NFS_OBJ@/$NFS_LINKED}
echo "[link-radv] NFS shares: $nfs_count C library calls wrapped (orbis-shims/OrbisNfs.wrap), chained with the platform's: ${chained[*]:-none}"
# qsort_r and localtime_r are bound by radv-bindings.S instead (see there).
bound=(mkstemps openlog popen pclose open_memstream __xuname __assert
  __memset_chk regcomp regexec regfree regerror newlocale freelocale
  strtod_l strtof_l dladdr utimensat localeconv_l strtoll_l strtoull_l strtold_l
  snprintf_l sscanf_l asprintf_l strcoll_l strxfrm_l strftime_l wcscoll_l wcsxfrm_l
  btowc_l wctob_l iswctype_l mbrlen_l mbrtowc_l mbsrtowcs_l mbsnrtowcs_l wcrtomb_l
  wcsnrtombs_l mbtowc_l ___mb_cur_max_l ___runetype_l ___tolower_l ___toupper_l
  __runes_for_locale catopen catgets catclose backtrace backtrace_symbols_fd
  __cxa_thread_atexit_impl
  arc4random arc4random_buf arc4random_uniform statvfs fstatvfs
  futimens
  opendir fdopendir readdir rewinddir dirfd closedir nl_langinfo nl_langinfo_l getpwuid_r
  posix_fallocate access
  openat unlinkat fchmodat fstatat mkdirat renameat memfd_create)
map="$OUT/build/radv-platform-local.map"
{
  printf '{\n    local:\n'
  for name in "${bound[@]}"; do
    radv_flags+=("--defsym=$name=ps5_$name")
    printf '        %s;\n' "$name"
  done
  printf '};\n'
} > "$map"
radv_flags+=(--version-script "$map")

# 4. The pie link (twice, as link-vk.sh: the second pass localizes everything but the imports). libunwind.a comes once more
# before the system modules' stubs for _Unwind_Backtrace: the platform's backtrace() walks with it, and the console libc's
# would mix two unwinders in one walk (RPCS3-PS5's link does the same; the RADV titles list libunwind before the stubs).
STUBS=$(find "$SDK/target/lib" -maxdepth 1 -name '*.so' ! -name 'libScePosixForWebKit.so' ! -name 'libSceGLSlimVSH.so' | sort)
ORDER_ARGS=()
ORDER_FILE=${ORBIS_ORDER:-$here/orbis-hot.order}
if [[ $ORDER_FILE != none && -f $ORDER_FILE ]]; then
  ORDER_ARGS=(--symbol-ordering-file="$ORDER_FILE" --no-warn-symbol-ordering)
fi
pie_link() {
  # shellcheck disable=SC2086
  "$LLD" -m elf_x86_64 -pie -z max-page-size=0x4000 -mllvm -emulated-tls \
    --hash-style=gnu -T "$NATIVE/tooling/native/ps5-pie.ld" -T "$here/orbis-shims/ehframe.ld" --eh-frame-hdr \
    --version-script "$NATIVE/tooling/native/app-symbols.map" "${ORDER_ARGS[@]}" "${radv_flags[@]}" "$@" -e _start \
    ${LLD_EXTRA:---no-dynamic-linker} \
    -o "$OUT/build/llvm-pie.elf" $OBJS \
    --whole-archive "$RADV_ARCHIVE" --no-whole-archive \
    "${GLSLANG_LIBS[@]}" \
    -u _Unwind_Backtrace "$SDK"/target/lib/libunwind.a \
    --as-needed $STUBS \
    -L "$SDK"/target/lib \
    "$SDK"/target/lib/libc++.a "$SDK"/target/lib/libc++abi.a \
    "$SDK"/target/lib/libunwind.a "$SDK"/target/lib/libc.a "$PLATFORM_LIB" "$RADV_BUILTINS" -lpthread
}
pie_link
echo "[link-radv] pie link OK"
python3 "$port/genmap.py" "$OUT/build/llvm-pie.elf" "$SDK/target/lib" "$OUT/build/orbis-exports.map"
pie_link --version-script "$OUT/build/orbis-exports.map"
echo "[link-radv] pie link (localized) OK"
"$NM" "$OUT/build/llvm-pie.elf" > "$OUT/build/llvm-pie.nm" # (not piped into grep -q: with pipefail, nm's SIGPIPE fails the test)
if ! grep -qE " [Tt] radv_GetInstanceProcAddr$" "$OUT/build/llvm-pie.nm"; then
  echo "[link-radv] error: RADV's entry point is not in the eboot (OrbisDriverIsRADV would say ps5vk)" >&2; exit 1
fi

# 5. Native link, console preparation and signing, as link-vk.sh.
if [[ ! -x $TOOL ]]; then
  mkdir -p "$(dirname -- "$TOOL")"
  "${HOST_CXX:-clang++-18}" -std=c++20 -O2 -o "$TOOL" \
    "$NATIVE"/tooling/native/{native_app_builder,elf_object,self_container,sce_module_writer}.cpp -lz
  echo "[link-radv] built $TOOL"
fi
"$TOOL" link --in "$OUT/build/llvm-pie.elf" --out "$OUT/build/eboot.elf" \
  --stub-dir "$SDK/target/lib" --module-sdk 0x02000009 \
  --companion-sdk 0x08050001 --file-name eboot.elf
echo "[link-radv] native link OK"
python3 "$PREPARE" --input "$OUT/build/eboot.elf" --output "$OUT/build/eboot-console-ready.elf"
"$TOOL" self --sign --in "$OUT/build/eboot.elf" --out "$OUT/app/eboot.bin" --magic 0x1D3D154F
"$TOOL" self --inspect --file "$OUT/app/eboot.bin"
sha256sum "$OUT/app/eboot.bin" "$OUT/build/eboot-console-ready.elf"
echo "[link-radv] signed eboot OK ($TAG, RADV)"
