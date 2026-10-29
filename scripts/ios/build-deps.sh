#!/usr/bin/env bash
# iOS proof of concept (branch ios-poc): the libraries the runtime needs, built static for
# arm64 iPhones into deps/ios-arm64, from the sources scripts/macos/build-deps.sh already fetched
# (deps/build/src). Run that first.
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}
src=$PWD/deps/build/src
prefix=$PWD/deps/ios-arm64
work=$PWD/deps/build-ios
jobs=$(sysctl -n hw.ncpu)
ios=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64
     -DCMAKE_OSX_DEPLOYMENT_TARGET=18.0 -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_FIND_ROOT_PATH="$prefix"
     -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON)
mkdir -p "$prefix" "$work"
package() { # package NAME SOURCE [cmake args]: configure, build and install unless installed
    local name=$1 source=$2; shift 2
    [[ -f $work/$name.done ]] && return
    echo "== $name"
    cmake -S "$source" -B "$work/$name" "${ios[@]}" "$@" >/dev/null
    cmake --build "$work/$name" -j "$jobs" >/dev/null
    cmake --install "$work/$name" >/dev/null
    touch "$work/$name.done"
}
package sdl3 "$src/sdl3" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
package zycore "$src/zydis/dependencies/zycore" -DZYCORE_BUILD_SHARED_LIB=OFF
# As in the macOS script: the installed Zycore target may lack its include directory.
package zydis "$src/zydis" -DZYAN_SYSTEM_ZYCORE=ON -DZYDIS_BUILD_SHARED_LIB=OFF -DCMAKE_C_FLAGS="-I$prefix/include" \
    -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_EXAMPLES=OFF -DZYDIS_BUILD_DOXYGEN=OFF
# The GPU library's (gpu/CMakeLists.txt), with the macOS script's options.
package vulkan-headers "$src/vulkan-headers"
package spirv-cross "$src/spirv-cross" -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_CLI=OFF \
    -DSPIRV_CROSS_ENABLE_TESTS=OFF -DSPIRV_CROSS_ENABLE_HLSL=OFF -DSPIRV_CROSS_ENABLE_CPP=OFF \
    -DSPIRV_CROSS_ENABLE_REFLECT=OFF -DSPIRV_CROSS_ENABLE_C_API=OFF -DSPIRV_CROSS_ENABLE_UTIL=OFF
package fmt "$src/fmt" -DFMT_TEST=OFF -DFMT_DOC=OFF
package magic-enum "$src/magic-enum" -DMAGIC_ENUM_OPT_BUILD_EXAMPLES=OFF -DMAGIC_ENUM_OPT_BUILD_TESTS=OFF
package robin-map "$src/robin-map"
package xbyak "$src/xbyak"
package vma "$src/vma"
package miniz "$src/miniz" -DBUILD_EXAMPLES=OFF -DBUILD_FUZZERS=OFF -DBUILD_TESTS=OFF
package xxhash "$src/xxhash/cmake_unofficial" -DXXHASH_BUILD_XXHSUM=OFF
package boost "$src/boost" -DBOOST_INCLUDE_LIBRARIES='asio;container;container_hash;icl;intrusive;pool;preprocessor' \
    -DBOOST_SKIP_INSTALL_RULES=OFF -DBOOST_CONTEXT_ARCHITECTURE=arm64 -DBOOST_CONTEXT_ABI=aapcs
# FFmpeg (AvPlayer, the game's movies): static, in a clean copy of its sources (the macOS build
# configured the original in place, and FFmpeg then refuses out-of-tree builds).
if [[ ! -f $work/ffmpeg.done ]]; then
    echo "== ffmpeg"
    sdk=$(xcrun -sdk iphoneos --show-sdk-path)
    rm -rf "$work/ffmpeg" && cp -R "$src/ffmpeg" "$work/ffmpeg"
    (cd "$work/ffmpeg" && { make distclean >/dev/null 2>&1 || true; } && ./configure --prefix="$prefix" --enable-cross-compile --arch=aarch64 \
        --target-os=darwin --cc="$(xcrun -sdk iphoneos -f clang)" --sysroot="$sdk" \
        --extra-cflags="-arch arm64 -miphoneos-version-min=18.0" \
        --extra-ldflags="-arch arm64 -miphoneos-version-min=18.0" \
        --enable-static --disable-shared --disable-programs --disable-doc --disable-avdevice \
        --disable-avfilter --disable-network --disable-autodetect --enable-videotoolbox >/dev/null &&
        make -j "$jobs" >/dev/null && make install >/dev/null)
    touch "$work/ffmpeg.done"
fi
echo "iOS dependencies: $prefix"
