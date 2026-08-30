#!/bin/bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CLASSPATH=$(cat "$SCRIPT_DIR/minecraft_classpath.txt"):$SCRIPT_DIR/java
JAVA_25=$(/usr/libexec/java_home -v 25)

echo "Compiling BlockChangeTracer..."
$JAVA_25/bin/javac -cp "$CLASSPATH" -d "$SCRIPT_DIR/java" "$SCRIPT_DIR/java/BlockChangeTracer.java"

echo "Running BlockChangeTracer..."
time $JAVA_25/bin/java \
    -XstartOnFirstThread \
    --enable-native-access=ALL-UNNAMED \
    --sun-misc-unsafe-memory-access=allow \
    -Xmx4G \
    -cp "$CLASSPATH" \
    BlockChangeTracer "$@"
