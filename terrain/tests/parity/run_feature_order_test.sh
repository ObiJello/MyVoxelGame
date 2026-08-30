#!/bin/bash
# Run script for Java Feature Order Test

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MINECRAFT_DIR="$HOME/Library/Application Support/minecraft"
VERSION="26.1-snapshot-1"
MINECRAFT_JAR="$MINECRAFT_DIR/versions/$VERSION/$VERSION.jar"
LIBRARIES_DIR="$MINECRAFT_DIR/libraries"

# Check if classpath file exists
if [ ! -f "$SCRIPT_DIR/minecraft_classpath.txt" ]; then
    echo "Classpath file not found. Running build script first..."
    "$SCRIPT_DIR/build_minecraft_async_test.sh"
fi

# Compile FeatureOrderTest.java if needed
if [ ! -f "$SCRIPT_DIR/java/FeatureOrderTest.class" ] || [ "$SCRIPT_DIR/java/FeatureOrderTest.java" -nt "$SCRIPT_DIR/java/FeatureOrderTest.class" ]; then
    echo "Compiling FeatureOrderTest.java..."
    CLASSPATH=$(cat "$SCRIPT_DIR/minecraft_classpath.txt")
    javac -cp "$CLASSPATH" -d "$SCRIPT_DIR/java/" "$SCRIPT_DIR/java/FeatureOrderTest.java"
fi

# Read classpath
CLASSPATH=$(cat "$SCRIPT_DIR/minecraft_classpath.txt")
CLASSPATH="$CLASSPATH:$SCRIPT_DIR/java"

# Default output
OUTPUT="$SCRIPT_DIR/../output/java_feature_order.txt"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --output)
            OUTPUT="$2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Create output directory if needed
mkdir -p "$(dirname "$OUTPUT")"

echo "Running Java Feature Order Test..."
echo "Output: $OUTPUT"
echo ""

# Run with proper Java settings
time java \
    -XstartOnFirstThread \
    --enable-native-access=ALL-UNNAMED \
    --sun-misc-unsafe-memory-access=allow \
    -Xmx4G \
    -cp "$CLASSPATH" \
    FeatureOrderTest \
    --output "$OUTPUT"
