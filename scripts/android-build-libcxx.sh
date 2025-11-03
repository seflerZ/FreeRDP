#!/bin/bash

# ==== 配置 ====
# 你可以通过环境变量覆盖这些值
SCM_URL=""  # libc++ 是 NDK 内置的，无需 SCM
SCM_TAG=""
SCM_HASH=""

# 注意：libc++ 源码在 NDK 内部，无需 clone
LIBCXX_SRC="$ANDROID_NDK/sources/cxx-stl/llvm-libc++/libcxx"
LIBCXX_ABI_LIB_SRC="$ANDROID_NDK/sources/cxx-stl/llvm-libc++/libcxxabi"

source $(dirname "${BASH_SOURCE[0]}")/android-build-common.sh

function build_libcxx {
    local ARCH_ABI=$1      # 如 "arm64-v8a"
    local OARCH=$2         # 如 "arm64"
    local API_LEVEL=$3     # 如 21

    echo "Building libc++_shared.so for $ARCH_ABI (16KB page size)..."

    local BASE=$(pwd)
    local BUILD_DIR="$BUILD_SRC/build-$ARCH_ABI-16k"
    local INSTALL_DIR="$BUILD_SRC/libs/$OARCH"

    # 确保目录干净
    common_run rm -rf "$BUILD_DIR"
    common_run mkdir -p "$BUILD_DIR"
    common_run cd "$BUILD_DIR"

    # 设置 CMake 工具链
    local TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake"

    # 构建 libc++_shared.so with 16KB page alignment
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
        -DLIBCXX_HAS_MUSL_LIBC=OFF \
        -DLIBCXX_CXX_ABI=libcxxabi \
        -DLIBCXX_CXX_ABI_INCLUDE_PATHS="$LIBCXX_ABI_LIB_SRC/include" \
        -DANDROID_PAGE_SIZE=16384 \          # 👈 关键：16KB 页面
        "$LIBCXX_SRC"

    common_run make -j$(nproc)

    # 安装到临时目录
    common_run mkdir -p "$INSTALL_DIR/lib"
    common_run cp -f "lib/libc++_shared.so" "$INSTALL_DIR/lib/"

    common_run cd "$BASE"
}

# ==== 主流程 ====

# 解析参数（复用你的 common 脚本）
common_parse_arguments $@
common_check_requirements

# 不需要 clone，直接检查 NDK 是否包含 libc++ 源码
if [ ! -d "$LIBCXX_SRC" ]; then
    echo "Error: libc++ source not found in NDK at $LIBCXX_SRC"
    exit 1
fi

# 遍历架构
for ARCH in $BUILD_ARCH
do
    case $ARCH in
        "armeabi"|"armeabi-v7a")
            OARCH="arm"
            ABI="armeabi-v7a"
            ;;
        "arm64-v8a")
            OARCH="arm64"
            ABI="arm64-v8a"
            ;;
        "x86")
            OARCH="x86"
            ABI="x86"
            ;;
        "x86_64")
            OARCH="x86_64"
            ABI="x86_64"
            ;;
        *)
            echo "Unsupported arch: $ARCH"
            continue
            ;;
    esac

    echo "Building for $ARCH (ABI: $ABI, OARCH: $OARCH)"

    # 只有 arm64-v8a 需要 16KB 版本（16KB 仅影响 arm64）
    # 但为了结构统一，我们为所有 arch 构建（4KB），仅 arm64 加 16KB 参数
    if [ "$ARCH" = "arm64-v8a" ]; then
        build_libcxx "$ABI" "$OARCH" "$NDK_TARGET"
    else
        # 对于非 arm64，构建标准 4KB 版本（可选）
        # 如果你只需要 16KB，可跳过
        echo "Skipping non-arm64 architecture for 16KB build."
        continue
    fi

    # 复制到输出目录（与 openh264 脚本结构一致）
    if [ ! -d "$BUILD_DST/$ARCH" ]; then
        common_run mkdir -p "$BUILD_DST/$ARCH"
    fi

    common_run cp -f "$BUILD_SRC/libs/$OARCH/lib/libc++_shared.so" "$BUILD_DST/$ARCH/"
done

echo "libc++_shared.so build completed."