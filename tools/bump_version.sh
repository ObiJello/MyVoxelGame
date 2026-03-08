#!/bin/bash
# bump_version.sh - Auto-increment build number and update version in source files
# Usage: bump_version.sh <type> <build_number_file> <major> <minor> <source_root>
#   type: "launcher" or "game"
#   build_number_file: path to file storing the integer build number
#   major: major version number
#   minor: minor version number
#   source_root: path to the project source root

TYPE="$1"
NUMBER_FILE="$2"
MAJOR="$3"
MINOR="$4"
SOURCE_ROOT="$5"

# Read current build number (or 0)
N=$(cat "$NUMBER_FILE" 2>/dev/null || echo "0")
N=$((N + 1))
echo "$N" > "$NUMBER_FILE"

VERSION="${MAJOR}.${MINOR}.${N}"

if [ "$TYPE" = "launcher" ]; then
    # Update LauncherConfig.hpp directly
    CONFIG="$SOURCE_ROOT/src/launcher/LauncherConfig.hpp"
    sed -i '' "s|LauncherVersion = \"[^\"]*\"|LauncherVersion = \"${VERSION}\"|" "$CONFIG"
else
    # Update Config.hpp window title
    CONFIG="$SOURCE_ROOT/src/common/core/Config.hpp"
    sed -i '' "s|GAME_VERSION \"[^\"]*\"|GAME_VERSION \"${VERSION}\"|" "$CONFIG"
fi
