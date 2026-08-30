#!/bin/bash
# Build script for Minecraft Async Chunk Test
# Uses Minecraft's actual server infrastructure with tickets

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MINECRAFT_DIR="$HOME/Library/Application Support/minecraft"
VERSION="26.1-snapshot-1"
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

# Build classpath from all jars in libraries directory
echo "Building classpath from all library jars..."
CLASSPATH="$MINECRAFT_JAR"

# Find all jar files in the libraries directory
JAR_COUNT=0
while IFS= read -r jar; do
    CLASSPATH="$CLASSPATH:$jar"
    ((JAR_COUNT++))
done < <(find "$LIBRARIES_DIR" -name "*.jar" -type f 2>/dev/null)

echo "Found $JAR_COUNT library jars"

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
    echo "WARNING: Minecraft 26.1 requires Java 21+. Current: $JAVA_VERSION"
fi

# Compile - use Java 25 features since Minecraft 26.1 requires it
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
