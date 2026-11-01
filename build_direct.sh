#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT:-/home/user/android-ndk-r29}"
PRELOADER_LIBRARY="${PRELOADER_LIBRARY:-/home/user/levi-preloader-libs/libpreloader-1525.so}"
PRELOADER_INCLUDE="${PRELOADER_INCLUDE:-/home/user/preloader/include}"
OUT="${OUT:-${ROOT}/build}"
mkdir -p "$OUT"

if [ ! -x "${ANDROID_NDK_ROOT}/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++" ]; then
    if [ -f /home/user/restore_ndk.sh ]; then
        bash /home/user/restore_ndk.sh
    fi
fi

TOOLCHAIN="${ANDROID_NDK_ROOT}/toolchains/llvm/prebuilt/linux-x86_64"
CXX="${TOOLCHAIN}/bin/clang++"
SYSROOT="${TOOLCHAIN}/sysroot"
API28="${SYSROOT}/usr/lib/aarch64-linux-android/28"
AARCH64_LIB="${SYSROOT}/usr/lib/aarch64-linux-android"
LIBCXX_SHARED="${AARCH64_LIB}/libc++_shared.so"

[ -d "$PRELOADER_INCLUDE/pl" ] || { echo "error: preloader headers not found at $PRELOADER_INCLUDE" >&2; exit 1; }
[ -f "$PRELOADER_LIBRARY" ] || { echo "error: preloader library not found at $PRELOADER_LIBRARY" >&2; exit 1; }

cd "$ROOT"

"$CXX" \
  --target=aarch64-linux-android28 \
  --sysroot="$SYSROOT" \
  -std=c++20 -O2 -fPIC -shared \
  -fvisibility=hidden \
  -ffunction-sections -fdata-sections \
  -Wall -Wextra -Wpedantic \
  -Wl,--no-undefined -Wl,--gc-sections -Wl,--build-id=sha1 \
  -Wl,-soname,libElytraChestplate.so \
  -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 \
  -Wl,-z,relro -Wl,-z,now \
  -I"${ROOT}/src" -I"${PRELOADER_INCLUDE}" \
  src/ElytraChestplate.cpp \
  -L"$(dirname "$PRELOADER_LIBRARY")" -L"$API28" -L"$AARCH64_LIB" \
  "$PRELOADER_LIBRARY" -landroid -llog -ldl -lm -lc \
  "$LIBCXX_SHARED" \
  -o "${OUT}/libElytraChestplate.so"

cp "${OUT}/libElytraChestplate.so" "${ROOT}/libElytraChestplate.so"
echo "Built ${OUT}/libElytraChestplate.so ($(stat -c%s "${OUT}/libElytraChestplate.so") bytes)"

(cd "${ROOT}" && zip -q -9 "${OUT}/ElytraChestplate-1.0.0.levipack" manifest.json libElytraChestplate.so)
cp "${OUT}/ElytraChestplate-1.0.0.levipack" "${ROOT}/ElytraChestplate-1.0.0.levipack"
echo "Packaged ${ROOT}/ElytraChestplate-1.0.0.levipack ($(stat -c%s "${ROOT}/ElytraChestplate-1.0.0.levipack") bytes)"
