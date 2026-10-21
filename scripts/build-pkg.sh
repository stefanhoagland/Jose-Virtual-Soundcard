#!/usr/bin/env bash
# Builds the installer dist/JoseVirtualSoundcard-<version>.pkg with the app
# (to /Applications) and the Core Audio driver (to /Library/Audio/Plug-Ins/HAL).
# Run scripts/build-app.sh and scripts/build-driver.sh first.
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=${VERSION:-0.0.0}
ROOT=build/pkgroot
SCRIPTS=build/pkgscripts
rm -rf "$ROOT" "$SCRIPTS"
mkdir -p "$ROOT/Applications" "$ROOT/Library/Audio/Plug-Ins/HAL" "$SCRIPTS"
cp -R "dist/Jose Virtual Soundcard.app" "$ROOT/Applications/"
cp -R build/JoseVirtualSoundcard.driver "$ROOT/Library/Audio/Plug-Ins/HAL/"

cat > "$SCRIPTS/postinstall" <<'SH'
#!/bin/sh
# Restart Core Audio so the new driver loads (sound stops for a moment).
launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || killall coreaudiod 2>/dev/null
exit 0
SH
chmod +x "$SCRIPTS/postinstall"

# Always install to the fixed places, even if an older copy of the app sits elsewhere.
pkgbuild --analyze --root "$ROOT" build/components.plist
i=0
while plutil -extract $i xml1 -o /dev/null build/components.plist 2>/dev/null; do
  plutil -replace $i.BundleIsRelocatable -bool NO build/components.plist
  i=$((i + 1))
done

pkgbuild --root "$ROOT" --component-plist build/components.plist --scripts "$SCRIPTS" \
  --identifier com.josevirtualsoundcard.pkg --version "$VERSION" --install-location / \
  "dist/JoseVirtualSoundcard-$VERSION.pkg"
pkgutil --payload-files "dist/JoseVirtualSoundcard-$VERSION.pkg" | head -20
echo "Built dist/JoseVirtualSoundcard-$VERSION.pkg"
