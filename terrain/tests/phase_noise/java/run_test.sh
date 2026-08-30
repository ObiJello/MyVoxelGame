#!/bin/bash

# Multi-Chunk Phase Noise Test Runner
# Compiles and runs the Java test with all Minecraft dependencies

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MINECRAFT_DIR="$HOME/Library/Application Support/minecraft"
MC_VERSION="26.1-snapshot-1"
MC_JAR="$MINECRAFT_DIR/versions/$MC_VERSION/$MC_VERSION.jar"
LIBS_DIR="$MINECRAFT_DIR/libraries"

echo "=== Java Multi-Chunk Phase Noise Test Runner ==="
echo "Minecraft JAR: $MC_JAR"
echo "Libraries: $LIBS_DIR"
echo

# Check Minecraft JAR exists
if [ ! -f "$MC_JAR" ]; then
    echo "ERROR: Minecraft JAR not found: $MC_JAR"
    exit 1
fi

# Build classpath from all JARs in libraries folder
echo "Building classpath from all library JARs..."
CLASSPATH="$MC_JAR"

# Find all JAR files recursively in the libraries folder
JAR_COUNT=0
while IFS= read -r -d '' jar; do
    CLASSPATH="$CLASSPATH:$jar"
    ((JAR_COUNT++))
done < <(find "$LIBS_DIR" -name "*.jar" -print0)

echo "Found $JAR_COUNT library JARs"
echo

# Add current directory to classpath
CLASSPATH="$CLASSPATH:$SCRIPT_DIR"

# Compile the test
echo "Compiling MultiChunkPhaseNoiseTest.java..."
cd "$SCRIPT_DIR"

javac -cp "$CLASSPATH" \
    -d "$SCRIPT_DIR" \
    -Xlint:none \
    MultiChunkPhaseNoiseTest.java 2>&1

if [ $? -ne 0 ]; then
    echo "ERROR: Compilation failed"
    exit 1
fi

echo "Compilation successful!"
echo

# Run the test
echo "Running test..."
echo "----------------------------------------"

java -cp "$CLASSPATH" \
    -Xmx4G \
    -Dlog4j.configurationFile= \
    MultiChunkPhaseNoiseTest "$@"

EXIT_CODE=$?

echo "----------------------------------------"
if [ $EXIT_CODE -eq 0 ]; then
    echo "Test completed successfully!"
else
    echo "Test exited with code: $EXIT_CODE"
fi

exit $EXIT_CODE
