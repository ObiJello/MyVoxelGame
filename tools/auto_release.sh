#!/bin/bash
# auto_release.sh - Called by CMake POST_BUILD to zip and upload to GitHub
# Usage: auto_release.sh <type> <build_number_file> <major> <minor> <app_name> <bin_dir> <repo>
# Non-fatal: if anything fails, exit 0 so the build still succeeds

TYPE="$1"
NUMBER_FILE="$2"
MAJOR="$3"
MINOR="$4"
APP_NAME="$5"
BIN_DIR="$6"
REPO="$7"

# Read build number
N=$(cat "$NUMBER_FILE" 2>/dev/null || echo "0")
VERSION="${MAJOR}.${MINOR}.${N}"

if [ "$TYPE" = "launcher" ]; then
    TAG="launcher-v${VERSION}"
    ZIP_NAME="ObeyCraftLauncher-v${VERSION}-macos-universal.zip"
else
    TAG="v${VERSION}"
    ZIP_NAME="ObeyCraft-v${VERSION}-macos-universal.zip"
fi

cd "$BIN_DIR" || exit 0

# Clean old zips first
if [ "$TYPE" = "launcher" ]; then
    rm -f ObeyCraftLauncher-v*-macos-universal.zip 2>/dev/null
else
    rm -f ObeyCraft-v*-macos-universal.zip 2>/dev/null
fi

# Zip
zip -r "$ZIP_NAME" "$APP_NAME" > /dev/null 2>&1 || exit 0
echo "[auto-release] Zipped: $ZIP_NAME"

# Upload to GitHub (non-fatal)
if command -v gh &> /dev/null; then
    if gh release view "$TAG" --repo "$REPO" &>/dev/null 2>&1; then
        gh release upload "$TAG" "$BIN_DIR/$ZIP_NAME" --repo "$REPO" --clobber 2>/dev/null || true
        echo "[auto-release] Updated release: $TAG"
    else
        gh release create "$TAG" \
            --repo "$REPO" \
            --title "$TAG" \
            --notes "Auto-release $TAG" \
            "$BIN_DIR/$ZIP_NAME" 2>/dev/null || true
        echo "[auto-release] Created release: $TAG"
    fi
else
    echo "[auto-release] gh CLI not found - zip created but not uploaded"
fi

exit 0
