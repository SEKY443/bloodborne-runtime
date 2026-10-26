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
echo "iOS dependencies: $prefix"
