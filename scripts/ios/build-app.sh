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
touch ios/app_main.c
xcodebuild -project ios/BloodbornePoC.xcodeproj -scheme BloodbornePoC -configuration Debug \
    -destination "id=$BB_IOS_DEVICE" -derivedDataPath out/ios/xcode -allowProvisioningUpdates \
    DEVELOPMENT_TEAM="$BB_IOS_TEAM" build 2>&1 | grep -E "error:|BUILD (SUCCEEDED|FAILED)"
xcrun devicectl device install app --device "$BB_IOS_DEVICE" \
    out/ios/xcode/Build/Products/Debug-iphoneos/BloodbornePoC.app 2>&1 | grep -E "App installed|rror"
