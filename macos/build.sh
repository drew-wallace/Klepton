#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/installer-macos"
NODE_EXECUTABLE="$(command -v node || true)"
xcodebuild -quiet -project "$ROOT/macos/KleptonInstaller.xcodeproj" -scheme KleptonInstaller \
  -configuration Debug -destination 'platform=macOS,arch=arm64' -derivedDataPath "$OUT" \
  "INFOPLIST_KEY_KleptonCheckout=$ROOT" "INFOPLIST_KEY_KleptonNodeExecutable=$NODE_EXECUTABLE" build
APP="$OUT/Build/Products/Debug/KleptonInstaller.app"
echo "$APP"
if [ "${1:-}" = --open ]; then open -n "$APP"; fi
