#!/bin/bash
# create_release.sh - Build, package, and upload a new game release to GitHub
#
# Usage:
#   ./tools/create_release.sh v0.2.0 "Release notes here"
#
# Requirements:
#   - cmake, ninja
#   - gh (GitHub CLI, authenticated)
#   - zip
#
# The script will:
#   1. Build the game in Release mode
#   2. Create a zip archive named with platform and architecture
#   3. Create a GitHub release and upload the zip
#
# Asset naming convention:
#   ObeyCraft-v0.2.0-macos-arm64.zip
#   ObeyCraft-v0.2.0-macos-universal.zip
#   ObeyCraft-v0.2.0-windows-x64.zip

set -euo pipefail

VERSION="${1:-}"
NOTES="${2:-}"

if [ -z "$VERSION" ]; then
    echo "Usage: $0 <version> [release-notes]"
    echo "  Example: $0 v0.2.0 \"Bug fixes and performance improvements\""
    exit 1
fi

# Strip 'v' prefix for display
VERSION_NUM="${VERSION#v}"

# Detect platform and architecture
OS="$(uname -s)"
ARCH="$(uname -m)"

case "$OS" in
    Darwin)
        PLATFORM="macos"
        ;;
    Linux)
        PLATFORM="linux"
        ;;
    MINGW*|MSYS*|CYGWIN*)
        PLATFORM="windows"
        ;;
    *)
        echo "Unsupported platform: $OS"
        exit 1
        ;;
esac

case "$ARCH" in
    arm64|aarch64)
        ARCH_TAG="arm64"
        ;;
    x86_64|AMD64)
        ARCH_TAG="x86_64"
        ;;
    *)
        ARCH_TAG="$ARCH"
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build-release"

echo "=== ObeyCraft Release Builder ==="
echo "Version:      $VERSION"
echo "Platform:     $PLATFORM-$ARCH_TAG"
echo "Project dir:  $PROJECT_DIR"
echo "Build dir:    $BUILD_DIR"
echo ""

# Check if Vulkan SDK is set for universal builds
CMAKE_EXTRA_ARGS=""
if [ "$PLATFORM" = "macos" ] && [ -n "${VULKAN_SDK:-}" ]; then
    echo "Vulkan SDK detected: $VULKAN_SDK"
    echo "Building universal binary (arm64 + x86_64)..."
    ARCH_TAG="universal"
    CMAKE_EXTRA_ARGS="-DJALIN=ON"
fi

ASSET_NAME="ObeyCraft-${VERSION}-${PLATFORM}-${ARCH_TAG}.zip"

# Step 1: Configure
echo ""
echo ">>> Configuring..."
cmake -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    $CMAKE_EXTRA_ARGS \
    "$PROJECT_DIR"

# Step 2: Build
echo ""
echo ">>> Building..."
cmake --build "$BUILD_DIR" --target MyVoxelGame -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

# Step 3: Package
echo ""
echo ">>> Packaging as $ASSET_NAME..."

cd "$BUILD_DIR/bin"
if [ "$PLATFORM" = "macos" ]; then
    zip -r "$PROJECT_DIR/$ASSET_NAME" MyVoxelGame.app/
else
    zip -r "$PROJECT_DIR/$ASSET_NAME" MyVoxelGame* \
        -x "*.pdb" -x "*.ilk" -x "*.exp" -x "*.lib"
fi
cd "$PROJECT_DIR"

FILE_SIZE=$(du -sh "$ASSET_NAME" | cut -f1)
echo "Created: $ASSET_NAME ($FILE_SIZE)"

# Step 4: Create GitHub Release
echo ""
echo ">>> Creating GitHub release..."

if [ -z "$NOTES" ]; then
    NOTES="ObeyCraft $VERSION_NUM release"
fi

# Check if release already exists
if gh release view "$VERSION" &>/dev/null; then
    echo "Release $VERSION already exists. Uploading asset to existing release..."
    gh release upload "$VERSION" "$ASSET_NAME" --clobber
else
    gh release create "$VERSION" \
        --title "ObeyCraft $VERSION_NUM" \
        --notes "$NOTES" \
        "$ASSET_NAME"
fi

echo ""
echo "=== Release $VERSION published! ==="
echo "Asset: $ASSET_NAME"
echo ""
echo "You can view the release at:"
echo "  https://github.com/ObiJello/MyVoxelGame-Download/releases/tag/$VERSION"
