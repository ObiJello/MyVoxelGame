#!/bin/bash
# auto_release.sh - Called by CMake POST_BUILD to zip and upload to GitHub
# Usage: auto_release.sh <type> <build_number_file> <major> <minor> <app_name> <bin_dir> <repo> [obpatch]
# Non-fatal: if anything fails, exit 0 so the build still succeeds
#
# Game releases also get a binary patch (ObeyCraft-v<version>-binary.obpatch)
# from the previous game release's zip, made by the obpatch tool; the launcher
# applies it to the installed files instead of downloading the changed ones.
# The previous zip is kept in <bin_dir>/.release_cache (downloaded once when
# missing). The patch name must not contain a platform tag: launchers from
# before delta updates pick the asset whose name contains "macos".

TYPE="$1"
NUMBER_FILE="$2"
MAJOR="$3"
MINOR="$4"
APP_NAME="$5"
BIN_DIR="$6"
REPO="$7"
OBPATCH="$8"

# Read build number
N=$(cat "$NUMBER_FILE" 2>/dev/null || echo "0")
VERSION="${MAJOR}.${MINOR}.${N}"

if [ "$TYPE" = "launcher" ]; then
    TAG="launcher-mac-v${VERSION}"
    ZIP_NAME="ObeyCraftLauncher-v${VERSION}-macos-universal.zip"
else
    TAG="game-mac-v${VERSION}"
    ZIP_NAME="ObeyCraft-v${VERSION}-macos-universal.zip"
fi

cd "$BIN_DIR" || exit 0

# Clean old zips first
if [ "$TYPE" = "launcher" ]; then
    rm -f ObeyCraftLauncher-v*-macos-universal.zip 2>/dev/null
else
    rm -f ObeyCraft-v*-macos-universal.zip ObeyCraft-v*-binary.obpatch 2>/dev/null
fi

# Never publish a bundle whose signature does not verify. A broken seal still
# launches from the launcher (curl adds no quarantine), but Gatekeeper calls it
# "damaged" with no way past it once anything quarantines the app, and it is
# one changed byte away from dyld killing it with "Invalid Page" on Apple
# Silicon. Every release up to 0.1.149 shipped like this because the zip was
# taken before the build's re-sign step.
if ! VERIFY_OUTPUT=$(codesign --verify --deep --strict "$APP_NAME" 2>&1); then
    echo "[auto-release] ERROR: $APP_NAME fails codesign --verify --deep --strict; NOT releasing $TAG"
    echo "$VERIFY_OUTPUT" | sed 's/^/[auto-release]   /'
    exit 0
fi
if [ "$TYPE" = "game" ]; then
    PLIST_VERSION=$(/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$APP_NAME/Contents/Info.plist" 2>/dev/null)
    if [ "$PLIST_VERSION" != "$VERSION" ]; then
        echo "[auto-release] ERROR: $APP_NAME's Info.plist says '$PLIST_VERSION', expected $VERSION; NOT releasing $TAG"
        exit 0
    fi
fi

# Zip. Audio and images are already compressed: store them (-n) so installs
# do not spend time inflating data that deflate could not shrink.
zip -r -n .ogg:.png:.jpg:.zip "$ZIP_NAME" "$APP_NAME" > /dev/null 2>&1 || exit 0
echo "[auto-release] Zipped: $ZIP_NAME"

ASSETS=("$BIN_DIR/$ZIP_NAME")

# Binary patch from the previous game release (non-fatal: without it the
# launcher fetches the changed files from the zip).
if [ "$TYPE" = "game" ] && [ -n "$OBPATCH" ] && [ -x "$OBPATCH" ] && command -v gh &> /dev/null; then
    PREV_TAG=$(gh release list --repo "$REPO" --limit 200 --json tagName --jq '.[].tagName' 2>/dev/null \
        | grep '^game-mac-v[0-9]' | grep -vx "$TAG" \
        | sort -V | awk -v cur="$VERSION" '
            { v = $0; sub(/^game-mac-v/, "", v); split(v, a, "."); split(cur, c, ".");
              if (a[1] < c[1] || (a[1] == c[1] && (a[2] < c[2] || (a[2] == c[2] && a[3] < c[3])))) last = $0 }
            END { print last }')
    if [ -n "$PREV_TAG" ]; then
        PREV_VERSION="${PREV_TAG#game-mac-v}"
        CACHE="$BIN_DIR/.release_cache"
        PREV_ZIP="$CACHE/ObeyCraft-v${PREV_VERSION}-macos-universal.zip"
        mkdir -p "$CACHE"
        if [ ! -f "$PREV_ZIP" ]; then
            echo "[auto-release] Downloading previous release $PREV_TAG for the binary patch..."
            gh release download "$PREV_TAG" --repo "$REPO" --pattern "*macos-universal.zip" \
                --dir "$CACHE" --clobber > /dev/null 2>&1 || rm -f "$PREV_ZIP"
        fi
        if [ -f "$PREV_ZIP" ]; then
            PATCH_NAME="ObeyCraft-v${VERSION}-binary.obpatch"
            echo "[auto-release] Diffing against $PREV_TAG..."
            "$OBPATCH" make "$PREV_ZIP" "$BIN_DIR/$ZIP_NAME" "$BIN_DIR/$PATCH_NAME"
            if [ $? -eq 0 ] && "$OBPATCH" verify "$PREV_ZIP" "$BIN_DIR/$ZIP_NAME" "$BIN_DIR/$PATCH_NAME" > /dev/null; then
                ASSETS+=("$BIN_DIR/$PATCH_NAME")
                echo "[auto-release] Patch: $PATCH_NAME ($(du -h "$BIN_DIR/$PATCH_NAME" | cut -f1))"
            else
                rm -f "$BIN_DIR/$PATCH_NAME"
                echo "[auto-release] No binary patch for this release"
            fi
        fi
    fi
fi

# Upload to GitHub (non-fatal)
if command -v gh &> /dev/null; then
    if gh release view "$TAG" --repo "$REPO" &>/dev/null 2>&1; then
        gh release upload "$TAG" "${ASSETS[@]}" --repo "$REPO" --clobber 2>/dev/null || true
        echo "[auto-release] Updated release: $TAG"
    else
        gh release create "$TAG" \
            --repo "$REPO" \
            --title "$TAG" \
            --notes "Auto-release $TAG" \
            "${ASSETS[@]}" 2>/dev/null || true
        echo "[auto-release] Created release: $TAG"
    fi

    # The next release diffs against this one: keep its zip, and only its.
    if [ "$TYPE" = "game" ]; then
        CACHE="$BIN_DIR/.release_cache"
        mkdir -p "$CACHE"
        rm -f "$CACHE"/ObeyCraft-v*-macos-universal.zip
        cp -c "$BIN_DIR/$ZIP_NAME" "$CACHE/" 2>/dev/null || cp "$BIN_DIR/$ZIP_NAME" "$CACHE/"
    fi
else
    echo "[auto-release] gh CLI not found - zip created but not uploaded"
fi

exit 0
