#!/usr/bin/env bash
# Builds the Core Audio driver bundle: build/JoseVirtualSoundcard.driver
# (Apple Silicon + Intel). Install it with scripts/install-driver.sh or the .pkg.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=${VERSION:-0.0.0}
BUILD=${BUILD:-1}
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" "-DJVS_VERSION=$VERSION"
cmake --build build -j4 --target jvs_driver

B=build/JoseVirtualSoundcard.driver
rm -rf "$B"
mkdir -p "$B/Contents/MacOS" "$B/Contents/Resources"
cp build/JoseVirtualSoundcard "$B/Contents/MacOS/JoseVirtualSoundcard"
sed -e "s/__VERSION__/$VERSION/" -e "s/__BUILD__/$BUILD/" driver/Info.plist > "$B/Contents/Info.plist"
plutil -lint "$B/Contents/Info.plist"
lipo -info "$B/Contents/MacOS/JoseVirtualSoundcard"
# Ad-hoc signature (no paid Apple developer account yet).
codesign --force --sign - "$B"
codesign --verify --verbose "$B"
echo "Built $B"
