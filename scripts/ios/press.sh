#!/usr/bin/env bash
# iOS proof of concept (branch ios-poc): tools/press.sh for the iPhone: holds pad buttons in the
# running app (its BB_PAD_FILE is Documents/pad) for about HOLD seconds, then releases them.
#   BB_IOS_DEVICE=<udid> scripts/ios/press.sh "cross" [HOLD]     (tokens as in runtime_pad.c)
# A copy over the cable takes about a second, so short presses last at least that long.
set -euo pipefail
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}
: "${BB_IOS_DEVICE:?the iPhone UDID}"
app=${BB_IOS_APP:-io.github.pablovsouza.bloodborne-poc}
tmp=$(mktemp -d)
put() { printf '%s\n' "$1" > "$tmp/pad"; xcrun devicectl device copy to --device "$BB_IOS_DEVICE" \
    --domain-type appDataContainer --domain-identifier "$app" --source "$tmp/pad" --destination Documents/pad >/dev/null; }
put "$1"
sleep "${2:-0.2}"
put ""
rm -f "$tmp/pad"; rmdir "$tmp"
