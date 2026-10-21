#!/usr/bin/env bash
# Builds "Jose Virtual Soundcard.app" (Apple Silicon + Intel) and a .dmg.
# Runs on macOS with the Xcode command-line tools and CMake.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=${VERSION:-0.1.0}
BUILD=${BUILD:-1}
APP_NAME="Jose Virtual Soundcard"
OUT=dist
APP="$OUT/$APP_NAME.app"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
cmake --build build -j4 --target jvs_engine
lipo -info build/libjvs_engine.a

rm -rf "$OUT"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

for arch in arm64 x86_64; do
  swiftc -O -target "$arch-apple-macos12.0" -parse-as-library \
    -import-objc-header app/Bridging.h -Xcc -Iengine/include \
    app/*.swift build/libjvs_engine.a -lc++ \
    -framework AudioToolbox -framework CoreFoundation -framework AppKit -framework SwiftUI \
    -o "build/JoseVirtualSoundcard-$arch"
done
lipo -create build/JoseVirtualSoundcard-arm64 build/JoseVirtualSoundcard-x86_64 \
  -o "$APP/Contents/MacOS/JoseVirtualSoundcard"

sed -e "s/__VERSION__/$VERSION/" -e "s/__BUILD__/$BUILD/" app/Info.plist > "$APP/Contents/Info.plist"
printf 'APPL????' > "$APP/Contents/PkgInfo"

# App icon, drawn from scratch so there are no image files to keep.
swift scripts/make-icon.swift build/icon.png
ICONSET=build/AppIcon.iconset
rm -rf "$ICONSET"; mkdir -p "$ICONSET"
for s in 16 32 128 256 512; do
  sips -z $s $s build/icon.png --out "$ICONSET/icon_${s}x${s}.png" >/dev/null
  sips -z $((s*2)) $((s*2)) build/icon.png --out "$ICONSET/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns"

# Ad-hoc signature: enough to run on Apple Silicon. A Developer ID
# signature (paid Apple account) would remove the first-launch warning.
codesign --force --deep --sign - "$APP"
codesign --verify --verbose "$APP"

# Disk image with a shortcut to Applications.
STAGE=build/dmg
rm -rf "$STAGE"; mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
hdiutil create -volname "$APP_NAME" -srcfolder "$STAGE" -ov -format UDZO "$OUT/JoseVirtualSoundcard-$VERSION.dmg"
echo "Built $OUT/JoseVirtualSoundcard-$VERSION.dmg"
