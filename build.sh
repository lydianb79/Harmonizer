#!/bin/bash
# Builds build/Harmonizer.app (release) for this Mac.   Usage: ./build.sh [run]
# Set ARCH=arm64 or ARCH=x86_64 to cross-build into build/<arch>/Harmonizer.app.
set -euo pipefail
cd "$(dirname "$0")"

ARCH="${ARCH:-}"
if [ -n "$ARCH" ]; then
    FLAGS=(-c release --arch "$ARCH")
    APP="build/$ARCH/Harmonizer.app"
else
    FLAGS=(-c release)
    APP=build/Harmonizer.app
fi

swift build "${FLAGS[@]}" --product Harmonizer
BIN="$(swift build "${FLAGS[@]}" --show-bin-path)/Harmonizer"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/Harmonizer"
cp Resources/Info.plist "$APP/Contents/Info.plist"
cp Resources/AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"  # regenerate with Tools/make_icon.swift
codesign --force --sign - "$APP" >/dev/null

echo "Built $APP"
if [ "${1:-}" = "run" ]; then open "$APP"; fi
