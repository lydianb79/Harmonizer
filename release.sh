#!/bin/bash
# Builds one zip per architecture for a release:
#   build/Harmonizer-<version>-macOS-arm64.zip   (Apple silicon)
#   build/Harmonizer-<version>-macOS-x86_64.zip  (Intel)
set -euo pipefail
cd "$(dirname "$0")"

VERSION="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' Resources/Info.plist)"
for arch in arm64 x86_64; do
    ARCH="$arch" ./build.sh
    ZIP="build/Harmonizer-$VERSION-macOS-$arch.zip"
    rm -f "$ZIP"
    ditto -c -k --keepParent "build/$arch/Harmonizer.app" "$ZIP"
    echo "$ZIP  ($(lipo -archs "build/$arch/Harmonizer.app/Contents/MacOS/Harmonizer"))"
done
