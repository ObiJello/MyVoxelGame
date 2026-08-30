#!/bin/bash
# Run Java Minecraft tests with all dependencies

# Minecraft paths
MC_VERSION_DIR="/Users/obey/Library/Application Support/minecraft/versions/26.1-snapshot-1"
MC_LIBRARIES="/Users/obey/Library/Application Support/minecraft/libraries"
MC_JAR="$MC_VERSION_DIR/26.1-snapshot-1.jar"

# Find all jars
CLASSPATH="$MC_JAR"
while IFS= read -r -d '' jar; do
    CLASSPATH="$CLASSPATH:$jar"
done < <(find "$MC_LIBRARIES" -name "*.jar" -print0 2>/dev/null)

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Add current directory to classpath
CLASSPATH=".:$CLASSPATH"

# Java file to compile/run (default or first argument)
JAVA_FILE="${1:-MultiChunkPhaseNoiseTest.java}"
CLASS_NAME="${JAVA_FILE%.java}"

echo "=== Minecraft Java Test Runner ==="
echo "Java file: $JAVA_FILE"
echo "Classpath entries: $(echo "$CLASSPATH" | tr ':' '\n' | wc -l | tr -d ' ')"
echo ""

# Compile
echo "Compiling $JAVA_FILE..."
if javac -cp "$CLASSPATH" "$JAVA_FILE" 2>&1; then
    echo "Compilation successful."
    echo ""

    # Run
    echo "Running $CLASS_NAME..."
    echo "========================================"
    java -cp "$CLASSPATH" -Xmx2G "$CLASS_NAME" "${@:2}"
    EXIT_CODE=$?
    echo "========================================"
    echo "Exit code: $EXIT_CODE"
else
    echo "Compilation failed!"
    exit 1
fi
