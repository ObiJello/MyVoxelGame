#!/bin/bash
# Build script for Minecraft Async Chunk Test
# Uses Minecraft's actual server infrastructure with tickets

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MINECRAFT_DIR="$HOME/Library/Application Support/minecraft"
# The parity target. 26.3-pre-2 matches the decompiled reference source in
# minecraft_code_26.3-pre-2/ (version.json there); override with MC_VERSION=...
VERSION="${MC_VERSION:-26.3-pre-2}"
MINECRAFT_JAR="$MINECRAFT_DIR/versions/$VERSION/$VERSION.jar"
LIBRARIES_DIR="$MINECRAFT_DIR/libraries"
JAVA_DIR="$SCRIPT_DIR/java"

echo "=== Building Minecraft Async Chunk Test ==="
echo "Minecraft JAR: $MINECRAFT_JAR"
echo "Libraries: $LIBRARIES_DIR"
echo ""

# Check if Minecraft jar exists
if [ ! -f "$MINECRAFT_JAR" ]; then
    echo "ERROR: Minecraft JAR not found at $MINECRAFT_JAR"
    exit 1
fi

# Classpath = the version's OWN library list (its <version>.json), not every
# jar under libraries/: that folder holds several versions of the same
# library side by side (one per installed game version), and a scan put
# whichever sorted first on the path.
echo "Building classpath from $VERSION.json..."
LIBS=$(python3 - "$MINECRAFT_DIR" "$VERSION" <<'PY'
import json, os, sys
home, v = sys.argv[1], sys.argv[2]
j = json.load(open(f"{home}/versions/{v}/{v}.json"))
out = []
for lib in j["libraries"]:
    # Mojang's rule semantics: with rules present a library is excluded
    # unless a rule matching this OS says "allow" (the last match wins).
    rules = lib.get("rules")
    allowed = not rules
    for rule in rules or []:
        if rule.get("os", {}).get("name") in (None, "osx"):
            allowed = rule["action"] == "allow"
    art = lib.get("downloads", {}).get("artifact")
    if not allowed or not art:
        continue
    path = f"{home}/libraries/{art['path']}"
    if not os.path.exists(path):
        sys.exit(f"missing library: {path} (launch {v} once in the launcher)")
    out.append(path)
print(":".join(out))
PY
)
CLASSPATH="$MINECRAFT_JAR:$LIBS"
echo "Found $(echo "$LIBS" | tr ':' '\n' | wc -l | tr -d ' ') library jars"

# Add output directory for compiled class
CLASSPATH="$CLASSPATH:$JAVA_DIR"

# Export classpath for use by other scripts
echo "$CLASSPATH" > "$SCRIPT_DIR/minecraft_classpath.txt"
echo "Classpath saved to minecraft_classpath.txt"

# Compile the test
echo ""
echo "Compiling MinecraftAsyncChunkTest.java..."

# Check Java version
JAVA_VERSION=$(java -version 2>&1 | head -1 | cut -d'"' -f2 | cut -d'.' -f1)
echo "Java version: $JAVA_VERSION"

if [ "$JAVA_VERSION" -lt 21 ]; then
    echo "WARNING: Minecraft $VERSION requires Java 25. Current: $JAVA_VERSION"
fi

# Compile - Minecraft 26.x requires Java 25
javac -cp "$CLASSPATH" \
    --enable-preview \
    --release 25 \
    -d "$JAVA_DIR" \
    "$JAVA_DIR/MinecraftAsyncChunkTest.java"

if [ $? -eq 0 ]; then
    echo "Compilation successful!"
    echo ""
    echo "To run the test:"
    echo "  cd $SCRIPT_DIR"
    echo "  ./run_minecraft_async_test.sh [--radius N] [--output path]"
else
    echo "Compilation failed!"
    exit 1
fi
