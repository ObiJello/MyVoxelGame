#!/bin/bash
# create_dmg.sh - Create a professional macOS DMG with "drag to Applications" layout
#
# Usage:
#   ./tools/create_dmg.sh                                    # Uses universal build
#   ./tools/create_dmg.sh /path/to/ObeyCraftLauncher.app     # Custom app path
#
# Output: ~/Desktop/ObeyCraftLauncher.dmg

set -euo pipefail

APP_PATH="${1:-}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Default to universal build
if [ -z "$APP_PATH" ]; then
    APP_PATH="$PROJECT_DIR/cmake-build-universal/bin/ObeyCraftLauncher.app"
fi

if [ ! -d "$APP_PATH" ]; then
    echo "Error: App not found at: $APP_PATH"
    echo "Usage: $0 [/path/to/ObeyCraftLauncher.app]"
    exit 1
fi

APP_NAME="ObeyCraft Launcher"
DMG_NAME="ObeyCraftLauncher"
DMG_OUTPUT="$HOME/Downloads/${DMG_NAME}.dmg"
DMG_TEMP="$HOME/Downloads/${DMG_NAME}_temp.dmg"
VOLUME_NAME="$APP_NAME"
STAGING_DIR=$(mktemp -d)

echo "=== ObeyCraft DMG Builder ==="
echo "App:    $APP_PATH"
echo "Output: $DMG_OUTPUT"
echo ""

# Clean up on exit
cleanup() {
    rm -rf "$STAGING_DIR"
    rm -f "$DMG_TEMP"
    # Detach any leftover volume
    hdiutil detach "/Volumes/$VOLUME_NAME" 2>/dev/null || true
}
trap cleanup EXIT

# Step 1: Stage the contents
echo ">>> Staging files..."
cp -R "$APP_PATH" "$STAGING_DIR/"
ln -s /Applications "$STAGING_DIR/Applications"

# Step 2: Calculate size (app size + 20MB padding)
APP_SIZE_KB=$(du -sk "$STAGING_DIR" | cut -f1)
DMG_SIZE_KB=$((APP_SIZE_KB + 20480))

# Step 3: Create temporary read-write DMG
echo ">>> Creating DMG..."
rm -f "$DMG_TEMP" "$DMG_OUTPUT"
hdiutil create \
    -srcfolder "$STAGING_DIR" \
    -volname "$VOLUME_NAME" \
    -fs HFS+ \
    -fsargs "-c c=64,a=16,e=16" \
    -format UDRW \
    -size "${DMG_SIZE_KB}k" \
    "$DMG_TEMP" \
    > /dev/null

# Step 4: Mount and customize with AppleScript
echo ">>> Customizing Finder layout..."
MOUNT_DIR="/Volumes/$VOLUME_NAME"

# Mount the DMG
hdiutil attach "$DMG_TEMP" -mountpoint "$MOUNT_DIR" > /dev/null

# Wait for mount
sleep 1

# Use AppleScript to set Finder view options
osascript <<APPLESCRIPT
tell application "Finder"
    tell disk "$VOLUME_NAME"
        open
        set current view of container window to icon view
        set toolbar visible of container window to false
        set statusbar visible of container window to false
        set bounds of container window to {100, 100, 640, 440}
        set theViewOptions to icon view options of container window
        set arrangement of theViewOptions to not arranged
        set icon size of theViewOptions to 128
        set text size of theViewOptions to 14
        set background color of theViewOptions to {8738, 8738, 12336}
        set position of item "ObeyCraftLauncher.app" of container window to {130, 160}
        set position of item "Applications" of container window to {410, 160}
        close
        open
        update without registering applications
        delay 1
        close
    end tell
end tell
APPLESCRIPT

# Make sure everything is written
sync

# Step 5: Detach
hdiutil detach "$MOUNT_DIR" > /dev/null

# Step 6: Convert to compressed read-only DMG
echo ">>> Compressing..."
hdiutil convert "$DMG_TEMP" \
    -format UDZO \
    -imagekey zlib-level=9 \
    -o "$DMG_OUTPUT" \
    > /dev/null

rm -f "$DMG_TEMP"

DMG_SIZE=$(du -sh "$DMG_OUTPUT" | cut -f1)
echo ""
echo "=== DMG created! ==="
echo "File: $DMG_OUTPUT"
echo "Size: $DMG_SIZE"
echo ""
echo "Users open the DMG and drag ObeyCraft Launcher to Applications."
