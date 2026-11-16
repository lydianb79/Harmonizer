#!/bin/bash
# Builds build/Harmonizer.app (release). Usage: ./build.sh [run]
set -euo pipefail
cd "$(dirname "$0")"

swift build -c release --product Harmonizer
BIN="$(swift build -c release --show-bin-path)/Harmonizer"

APP=build/Harmonizer.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/Harmonizer"
cp Resources/Info.plist "$APP/Contents/Info.plist"
cp Resources/AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"  # regenerate with Tools/make_icon.swift
codesign --force --sign - "$APP" >/dev/null

echo "Built $APP"
if [ "${1:-}" = "run" ]; then open "$APP"; fi
