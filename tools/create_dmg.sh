#!/bin/bash
# create_dmg.sh - Create a macOS DMG with drag-to-Applications layout
#
# Requires: brew install create-dmg
#
# Usage:
#   ./tools/create_dmg.sh                                    # Uses latest build
#   ./tools/create_dmg.sh /path/to/ObeyCraftLauncher.app     # Custom app path
#
# Output: ~/Downloads/ObeyCraftLauncher.dmg

set -euo pipefail

APP_PATH="${1:-}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# Default: find the most recently modified launcher app across all build dirs
if [ -z "$APP_PATH" ]; then
    APP_PATH=$(ls -dt "$PROJECT_DIR"/cmake-build-*/bin/ObeyCraftLauncher.app 2>/dev/null | head -1)
    if [ -z "$APP_PATH" ]; then
        echo "Error: No ObeyCraftLauncher.app found in any cmake-build-*/bin/ directory"
        exit 1
    fi
fi

if [ ! -d "$APP_PATH" ]; then
    echo "Error: App not found at: $APP_PATH"
    echo "Usage: $0 [/path/to/ObeyCraftLauncher.app]"
    exit 1
fi

if ! command -v create-dmg &>/dev/null; then
    echo "Error: create-dmg not found. Install with: brew install create-dmg"
    exit 1
fi

DMG_OUTPUT="$HOME/Downloads/ObeyCraftLauncher.dmg"

echo "=== ObeyCraft DMG Builder ==="
echo "App:    $APP_PATH"
echo "Output: $DMG_OUTPUT"
echo ""

rm -f "$DMG_OUTPUT"

create-dmg \
    --volname "ObeyCraft Launcher" \
    --window-pos 200 120 \
    --window-size 540 400 \
    --icon-size 128 \
    --icon "ObeyCraftLauncher.app" 130 180 \
    --app-drop-link 410 180 \
    --no-internet-enable \
    --hide-extension "ObeyCraftLauncher.app" \
    "$DMG_OUTPUT" \
    "$APP_PATH"

DMG_SIZE=$(du -sh "$DMG_OUTPUT" | cut -f1)
echo ""
echo "=== DMG created! ==="
echo "File: $DMG_OUTPUT"
echo "Size: $DMG_SIZE"
echo ""
echo "Users open the DMG and drag ObeyCraft Launcher to Applications."
