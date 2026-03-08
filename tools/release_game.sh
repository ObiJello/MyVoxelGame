#!/bin/bash
# release_game.sh - Auto-increment version, rebuild, zip, and upload game
#
# Usage:
#   ./tools/release_game.sh               # Auto-increment patch (0.1.0 → 0.1.1)
#   ./tools/release_game.sh minor         # Bump minor (0.1.5 → 0.2.0)
#   ./tools/release_game.sh major         # Bump major (0.2.3 → 1.0.0)
#   ./tools/release_game.sh --dry         # Do everything except upload
#
# Requires: cmake, ninja, gh (GitHub CLI, authenticated), zip
# Only builds universal (cmake-build-universal)

set -euo pipefail

BUMP_TYPE="${1:-patch}"
DRY_RUN=false
if [ "$BUMP_TYPE" = "--dry" ]; then
    BUMP_TYPE="patch"
    DRY_RUN=true
fi
if [ "${2:-}" = "--dry" ]; then
    DRY_RUN=true
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CMAKE_FILE="$PROJECT_DIR/CMakeLists.txt"
PLIST_FILE="$PROJECT_DIR/Info.plist"
CONFIG_FILE="$PROJECT_DIR/src/common/core/Config.hpp"
PLATFORM_FILE="$PROJECT_DIR/src/platform/PlatformMain.cpp"
REPO="ObiJello/MyVoxelGame-Download"

# Read current version from CMakeLists.txt
CURRENT=$(grep "project(MyVoxelGame" "$CMAKE_FILE" | grep -o 'VERSION [0-9.]*' | cut -d' ' -f2)
IFS='.' read -r MAJOR MINOR PATCH <<< "$CURRENT"

# Auto-increment
case "$BUMP_TYPE" in
    major)
        MAJOR=$((MAJOR + 1))
        MINOR=0
        PATCH=0
        ;;
    minor)
        MINOR=$((MINOR + 1))
        PATCH=0
        ;;
    patch|*)
        PATCH=$((PATCH + 1))
        ;;
esac

VERSION="${MAJOR}.${MINOR}.${PATCH}"
TAG="v${VERSION}"

echo "=== ObeyCraft Game Release ==="
echo "  $CURRENT → $VERSION"
echo "  Tag: $TAG"
echo ""

# Step 1: Bump version in all files
echo ">>> Bumping version..."

# CMakeLists.txt
sed -i '' "s|project(MyVoxelGame LANGUAGES C CXX VERSION [0-9.]*)|project(MyVoxelGame LANGUAGES C CXX VERSION ${VERSION})|" "$CMAKE_FILE"

# Info.plist (both CFBundleShortVersionString and CFBundleVersion)
sed -i '' "/<key>CFBundleShortVersionString<\/key>/{n;s|<string>[^<]*</string>|<string>${VERSION}</string>|;}" "$PLIST_FILE"
sed -i '' "/<key>CFBundleVersion<\/key>/{n;s|<string>[^<]*</string>|<string>${VERSION}</string>|;}" "$PLIST_FILE"

# Config.hpp window title
sed -i '' "s|WindowTitle = \"Obey's Game v[^\"]*\"|WindowTitle = \"Obey's Game v${MAJOR}.${MINOR}\"|" "$CONFIG_FILE"

# PlatformMain.cpp sentry release
sed -i '' "s|myvoxelgame@[0-9.]*|myvoxelgame@${VERSION}|" "$PLATFORM_FILE"

echo "  CMakeLists.txt ✓"
echo "  Info.plist ✓"
echo "  Config.hpp ✓"
echo "  PlatformMain.cpp ✓"
echo ""

# Step 2: Build universal
echo ">>> Building universal release..."
cmake "$PROJECT_DIR" \
    -B "$PROJECT_DIR/cmake-build-universal" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    > /dev/null 2>&1

cmake --build "$PROJECT_DIR/cmake-build-universal" \
    --target MyVoxelGame \
    -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" \
    2>&1 | tail -3

# Step 3: Zip
ZIP_NAME="ObeyCraft-${TAG}-macos-universal.zip"
cd "$PROJECT_DIR/cmake-build-universal/bin"
rm -f "$ZIP_NAME"
zip -r "$ZIP_NAME" MyVoxelGame.app/ > /dev/null
ZIP_SIZE=$(du -sh "$ZIP_NAME" | cut -f1)
echo ""
echo ">>> Packaged: $ZIP_NAME ($ZIP_SIZE)"

# Step 4: Upload
if [ "$DRY_RUN" = true ]; then
    echo ">>> Dry run - skipping upload"
else
    echo ">>> Uploading to GitHub..."
    ZIP_PATH="$PROJECT_DIR/cmake-build-universal/bin/$ZIP_NAME"
    if gh release view "$TAG" --repo "$REPO" &>/dev/null; then
        gh release upload "$TAG" "$ZIP_PATH" --repo "$REPO" --clobber
    else
        gh release create "$TAG" \
            --repo "$REPO" \
            --title "ObeyCraft $VERSION" \
            --notes "ObeyCraft $VERSION release" \
            "$ZIP_PATH"
    fi
    echo ""
    echo "=== Published! ==="
    echo "https://github.com/$REPO/releases/tag/$TAG"
fi
