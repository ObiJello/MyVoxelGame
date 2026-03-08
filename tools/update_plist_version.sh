#!/bin/bash
# update_plist_version.sh - Update Info.plist version from build number
# Usage: update_plist_version.sh <build_number_file> <plist_path> <major> <minor>

NUMBER_FILE="$1"
PLIST_PATH="$2"
MAJOR="$3"
MINOR="$4"

N=$(cat "$NUMBER_FILE" 2>/dev/null || echo "0")
VERSION="${MAJOR}.${MINOR}.${N}"

/usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString ${VERSION}" -c "Set :CFBundleVersion ${VERSION}" "$PLIST_PATH" 2>/dev/null || true
