#!/bin/bash

# 不需要 SCM 信息（libc++ 来自 NDK）
SCM_URL=""
SCM_TAG=""
SCM_HASH=""

SCRIPT_DIR=$(dirname "${BASH_SOURCE[0]}")
source "$SCRIPT_DIR/android-build-common.sh"

function build_libcxx {
    local ARCH_ABI=$1      # e.g. "arm64-v8a"
    local OARCH=$2         # e.g. "arm64"
    local API_LEVEL=$3     # e.g. 21

    echo "Building libc++_shared.so for $ARCH_ABI with 16KB page size..."

    local BASE=$(pwd)
    local BUILD_DIR="$BUILD_SRC/build-libcxx-$ARCH_ABI-16k"
    local INSTALL_DIR="$BUILD_SRC/libs/$OARCH"

    common_run rm -rf "$BUILD_DIR"
    common_run mkdir -p "$BUILD_DIR"
    common_run cd "$BUILD_DIR"

    local TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake"
    local LIBCXX_SRC="$ANDROID_NDK/sources/cxx-stl/llvm-libc++/libcxx"
    local LIBCXXABI_SRC="$ANDROID_NDK/sources/cxx-stl/llvm-libc++/libcxxabi"

    if [ ! -d "$LIBCXX_SRC" ]; then
        echo "ERROR: libc++ source not found in NDK