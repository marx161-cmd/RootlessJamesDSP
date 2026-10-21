#!/usr/bin/env bash
# Build libjamesdsp_engine_static.a for one variant, outside Gradle, with the
# exact configure arguments Gradle uses for the shipped RelWithDebInfo build
# (recovered from app/.cxx/RelWithDebInfo/*/arm64-v8a/metadata_generation_command.txt).
#
#   build_engine_variant.sh full    -> Lua bridge + 108-band meter (what the user runs)
#   build_engine_variant.sh audio   -> audio-only: no Lua, no meter, no file I/O
#
# Output: app/.cxx-variants/<variant>/arm64-v8a/libjamesdsp_engine_static.a
# Copy it to vendor/JamesDSP/proprietary/vendor/lib64/soundfx/ and run
# `m libjamesdspaidl` in the crDroid tree to get the matching libjamesdspaidl.so.
set -euo pipefail
VARIANT="${1:?variant: full|audio}"
case "$VARIANT" in
  full)  LUA=ON ;;
  audio) LUA=OFF ;;
  *) echo "unknown variant $VARIANT" >&2; exit 2 ;;
esac
SDK="${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}"
NDK="$SDK/ndk/27.0.12077973"
CMAKE="$SDK/cmake/3.22.1/bin/cmake"
NINJA="$SDK/cmake/3.22.1/bin/ninja"
SRC="$(cd "$(dirname "$0")" && pwd)"
APP="$(cd "$SRC/../../.." && pwd)"
B="$APP/.cxx-variants/$VARIANT/arm64-v8a"
mkdir -p "$B"
"$CMAKE" -H"$SRC" -B"$B" -GNinja \
  -DCMAKE_SYSTEM_NAME=Android -DCMAKE_SYSTEM_VERSION=26 \
  -DANDROID_PLATFORM=android-26 -DANDROID_ABI=arm64-v8a -DCMAKE_ANDROID_ARCH_ABI=arm64-v8a \
  -DANDROID_NDK="$NDK" -DCMAKE_ANDROID_NDK="$NDK" \
  -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
  -DCMAKE_MAKE_PROGRAM="$NINJA" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DJDSP_LUA_BRIDGE="$LUA" >/dev/null
"$NINJA" -C "$B" jamesdsp_static
OUT="$B/libjamesdsp_engine_static.a"
echo "variant=$VARIANT lua=$LUA"
sha256sum "$OUT"
# Proof of what is (not) inside: Lua VM symbols and the bands.bin writer.
NM="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm"
echo "lua symbols:      $("$NM" --defined-only "$OUT" 2>/dev/null | grep -c ' T lua' || true)"
echo "band_shm symbols: $("$NM" --defined-only "$OUT" 2>/dev/null | grep -c 'band_shm\|band_publish\|band_meter' || true)"
echo "lsp symbols:      $("$NM" --defined-only "$OUT" 2>/dev/null | grep -c ' T lsp_' || true)"
echo "fft_x2 symbols:   $("$NM" --defined-only "$OUT" 2>/dev/null | grep -c '_x2' || true)"
