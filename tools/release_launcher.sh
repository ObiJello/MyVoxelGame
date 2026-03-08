#!/bin/bash
# release_launcher.sh - Auto-increment version, rebuild, zip, and upload launcher
#
# Usage:
#   ./tools/release_launcher.sh           # Auto-increment patch (1.0.0 → 1.0.1)
#   ./tools/release_launcher.sh minor     # Bump minor (1.0.5 → 1.1.0)
#   ./tools/release_launcher.sh major     # Bump major (1.2.3 → 2.0.0)
#   ./tools/release_launcher.sh --dry     # Do everything except upload
#
# Requires: cmake, ninja, gh (GitHub CLI, authenticated), zip

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
CONFIG_FILE="$PROJECT_DIR/src/launcher/LauncherConfig.hpp"
REPO="ObiJello/MyVoxelGame-Download"

# Read current version
CURRENT=$(grep 'LauncherVersion' "$CONFIG_FILE" | grep -o '"[^"]*"' | tr -d '"')
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
TAG="launcher-v${VERSION}"

echo "=== ObeyCraft Launcher Release ==="
echo "  $CURRENT → $VERSION"
echo "  Tag: $TAG"
echo ""

# Step 1: Bump version
echo ">>> Bumping version..."
sed -i '' "s|LauncherVersion = \"[^\"]*\"|LauncherVersion = \"${VERSION}\"|" "$CONFIG_FILE"

# Step 2: Build
echo ">>> Building universal release..."
cmake "$PROJECT_DIR" \
    -B "$PROJECT_DIR/cmake-build-universal" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    > /dev/null 2>&1

cmake --build "$PROJECT_DIR/cmake-build-universal" \
    --target ObeyCraftLauncher \
    -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" \
    2>&1 | tail -3

# Step 3: Zip
ZIP_NAME="ObeyCraftLauncher-v${VERSION}-macos-universal.zip"
cd "$PROJECT_DIR/cmake-build-universal/bin"
rm -f "$ZIP_NAME"
zip -r "$ZIP_NAME" ObeyCraftLauncher.app/ > /dev/null
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
            --title "Launcher $VERSION" \
            --notes "Launcher update v$VERSION" \
            "$ZIP_PATH"
    fi
    echo ""
    echo "=== Published! ==="
    echo "https://github.com/$REPO/releases/tag/$TAG"
fi
