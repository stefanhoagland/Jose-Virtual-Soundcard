#!/usr/bin/env bash
# Installs build/JoseVirtualSoundcard.driver and restarts Core Audio. Needs sudo.
set -euo pipefail
cd "$(dirname "$0")/.."
DEST=/Library/Audio/Plug-Ins/HAL
mkdir -p "$DEST"
rm -rf "$DEST/JoseVirtualSoundcard.driver"
cp -R build/JoseVirtualSoundcard.driver "$DEST/"
chown -R root:wheel "$DEST/JoseVirtualSoundcard.driver"
launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || killall coreaudiod || true
sleep 3
echo "Installed to $DEST"
