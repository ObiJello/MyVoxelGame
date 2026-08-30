#!/bin/bash

# Phase Noise Test - Java Runner
# This script compiles and runs the Java test against actual Minecraft .class files

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Minecraft paths
MC_VERSION="26.1-snapshot-1"
MC_JAR="$HOME/Library/Application Support/minecraft/versions/$MC_VERSION/$MC_VERSION.jar"
MC_LIBS="$HOME/Library/Application Support/minecraft/libraries"

# Check if Minecraft jar exists
if [ ! -f "$MC_JAR" ]; then
    echo "ERROR: Minecraft jar not found at: $MC_JAR"
    exit 1
fi

# Build classpath with all library jars
echo "Building classpath..."
CLASSPATH="$MC_JAR"

# Find all jars in the libraries folder recursively
while IFS= read -r -d '' jar; do
    CLASSPATH="$CLASSPATH:$jar"
done < <(find "$MC_LIBS" -name "*.jar" -print0)

# Add current directory to classpath for output
CLASSPATH="$CLASSPATH:."

echo "Classpath contains $(echo "$CLASSPATH" | tr ':' '\n' | wc -l | tr -d ' ') jars"

# Compile the test
echo "Compiling PhaseNoiseTest.java..."
javac -cp "$CLASSPATH" -d . PhaseNoiseTest.java

if [ $? -ne 0 ]; then
    echo "ERROR: Compilation failed"
    exit 1
fi
echo "Compilation successful"

# Run the test
echo ""
echo "Running Phase Noise Test..."
echo "========================================"
java -cp "$CLASSPATH" -Xmx2G PhaseNoiseTest java_phase_noise_output.txt

echo ""
echo "========================================"
echo "Test complete. Output written to: java_phase_noise_output.txt"
