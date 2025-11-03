#!/bin/bash

SCM_URL=""
SCM_TAG=""
SCM_HASH="any"

SCRIPT_DIR=$(dirname "${BASH_SOURCE[0]}")
source $(dirname "${BASH_SOURCE[0]}")/android-build-common.sh

function build_libcxx {
    local ARCH_ABI="$1"
    local OARCH="$2"
    local API_LEVEL="$3"

    echo "Building libc++_shared.so for $ARCH_ABI with 16KB page size..."

    local BASE="$(pwd)"
    local BUILD_DIR="$BUILD_SRC/build-libcxx-$ARCH_ABI-16k"
    local INSTALL_DIR="$BUILD_SRC/libs/$OARCH"

    common_run rm -rf "$BUILD_DIR"
    common_run mkdir -p "$BUILD_DIR"
    common_run cd "$BUILD_DIR"

    local TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake"
    local LIBCXX_SRC="$ANDROID_NDK/sources/cxx-stl/llvm-libc++/libcxx"
    local LIBCXXABI_SRC="$ANDROID_NDK/sources/cxx-stl/llvm-libc++/libcxxabi"

    if [ ! -d "$LIBCXX_SRC" ]; then
        echo "ERROR: libc++ source not found in NDK at $LIBCXX_SRC" >&2
        exit 1
    fi

    common_run cmake \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        -DANDROID_ABI="$ARCH_ABI" \
        -DANDROID_PLATFORM="android-$API_LEVEL" \
        -DANDROID_STL="none" \
        -DCMAKE_BUILD_TYPE=Release \
        -DLIBCXX_ENABLE_SHARED=ON \
        -DLIBCXX_ENABLE_STATIC=OFF \
        -DLIBCXX_ENABLE_FILESYSTEM=OFF \
        -DLIBCXX_ENABLE_LOCALIZATION=OFF \
        -DLIBCXX_CXX_ABI=libcxxabi \
        -DLIBCXX_CXX_ABI_INCLUDE_PATHS="$LIBCXXABI_SRC/include" \
        -DANDROID_PAGE_SIZE=16384 \
        "$LIBCXX_SRC"

    common_run make -j"$(nproc)"

    common_run mkdir -p "$INSTALL_DIR/lib"
    common_run cp -f "lib/libc++_shared.so" "$INSTALL_DIR/lib/"

    common_run cd "$BASE"
}

# =============== Main ===============
common_parse_arguments "$@"

# Skip SCM update — libc++ is part of NDK
# common_update $SCM_URL $SCM_TAG $BUILD_SRC $SCM_HASH

for ARCH in $BUILD_ARCH; do
    if [ "$ARCH" != "arm64-v8a" ]; then
        echo "Skipping non-arm64 architecture: $ARCH"
        continue
    fi

    OARCH="arm64"
    echo "Building libc++ for $ARCH"
    build_libcxx "$ARCH" "$OARCH" "$NDK_TARGET"

    DEST_DIR="$BUILD_DST/$ARCH"
    common_run mkdir -p "$DEST_DIR"
    common_run cp -f "$BUILD_SRC/libs/$OARCH/lib/libc++_shared.so" "$DEST_DIR/"
done

echo "✅ libc++_shared.so (16KB) build completed."