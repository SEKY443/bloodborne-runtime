#!/usr/bin/env bash
# iOS proof of concept (branch ios-poc): the GPU library (gpu/, the Metal backend) for arm64
# iPhones as a static library, out/ios/gpu/libbbgpu.a, linked into the app (iOS has no
# libraries that look up the runtime's symbols when loaded, as libbbgpu.dylib does on macOS).
# Needs scripts/ios/build-deps.sh first. Host tools (glslang, Python) come from the Mac.
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}
prefix=$PWD/deps/ios-arm64
export PKG_CONFIG_LIBDIR=$prefix/lib/pkgconfig PKG_CONFIG_SYSROOT_DIR=
cmake -S gpu -B out/ios/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
    -DCMAKE_PREFIX_PATH="$prefix" -DCMAKE_FIND_ROOT_PATH="$prefix" -DBB_LTO=OFF -DBB_NET=OFF \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER >/dev/null
ninja -C out/ios/gpu bbgpu > out/ios/gpu-build.log 2>&1 ||
    { grep -m 5 -B 3 -A 6 'error:' out/ios/gpu-build.log >&2; echo "GPU library failed (out/ios/gpu-build.log)" >&2; exit 1; }
echo out/ios/gpu/libbbgpu.a
