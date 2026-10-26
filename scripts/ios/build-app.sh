#!/usr/bin/env bash
# iOS proof of concept (branch ios-poc): builds the runtime core, links and signs the app with the
# player's Apple team and installs it on the connected iPhone (its Documents stay).
#   BB_IOS_TEAM=XXXXXXXXXX BB_IOS_DEVICE=<udid> scripts/ios/build-app.sh
# Xcode does not see out/ios/libbbios.a as an input, so app_main.c is touched to force the link.
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}
: "${BB_IOS_TEAM:?the Apple team ID (Xcode → Settings → Accounts)}" "${BB_IOS_DEVICE:?the iPhone UDID (xcrun devicectl list devices)}"
bash scripts/ios/build-core.sh >/dev/null
# The link line: the runtime, the GPU library (scripts/ios/build-gpu.sh; BB_IOS_GPU=0: the CPU-only
# stand-ins in libbbios.a instead) and their libraries.
deps=$PWD/deps/ios-arm64/lib gpu=$PWD/out/ios/gpu
libs=("$PWD/out/ios/libbbios.a")
if [[ ${BB_IOS_GPU:-1} != 0 ]]; then
    bash scripts/ios/build-gpu.sh >/dev/null
    libs+=("$gpu/libbbgpu.a" "$gpu/third_party/sirit/src/libsirit.a"
           "$gpu"/third_party/fsr-vulkan/libffx_vulkan_*.a
           "$deps"/libspirv-cross-{msl,glsl,core}.a "$deps"/lib{fmt,miniz,xxhash}.a
           "$deps"/lib{avformat,avcodec,swscale,swresample,avutil}.a "$deps"/libboost_container.a)
fi
libs+=("$deps/libSDL3.a")
frameworks=()
for f in AVFoundation AudioToolbox CoreAudio CoreBluetooth CoreFoundation CoreGraphics CoreHaptics \
         CoreMedia CoreMotion CoreVideo Foundation GameController IOSurface Metal MetalFX OpenGLES \
         QuartzCore Security UIKit UniformTypeIdentifiers VideoToolbox; do frameworks+=(-framework "$f"); done
touch ios/app_main.c
xcodebuild -project ios/BloodbornePoC.xcodeproj -scheme BloodbornePoC -configuration Debug \
    -destination "id=$BB_IOS_DEVICE" -derivedDataPath out/ios/xcode -allowProvisioningUpdates \
    DEVELOPMENT_TEAM="$BB_IOS_TEAM" OTHER_LDFLAGS="${libs[*]} ${frameworks[*]} -lc++ -lz -lbz2 -liconv" \
    build 2>&1 | grep -E "error:|Undefined symbols|^  \"_|BUILD (SUCCEEDED|FAILED)" | head -40
xcrun devicectl device install app --device "$BB_IOS_DEVICE" \
    out/ios/xcode/Build/Products/Debug-iphoneos/BloodbornePoC.app 2>&1 | grep -E "App installed|rror"
