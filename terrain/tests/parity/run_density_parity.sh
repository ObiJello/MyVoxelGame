#!/bin/bash
# Density engine parity: Java (the real 26.3 jar, vanilla datapack) vs the
# C++ float engine over every noise_settings router and aquifer function.
# Needs build_minecraft_async_test.sh run once (classpath) and
# terrain/build/density_parity_test built.
#   ./run_density_parity.sh [seed]
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
SEED="${1:-12345}"
OUT="$SCRIPT_DIR/results/density"
mkdir -p "$OUT"
CP="$(cat "$SCRIPT_DIR/minecraft_classpath.txt")"
javac -cp "$CP" --release 25 -d "$SCRIPT_DIR/java" "$SCRIPT_DIR/java/DensityParityTest.java"
java -cp "$CP" DensityParityTest "$SEED" "$OUT/java.txt" "$OUT/java.bin" > "$OUT/java.log" 2>&1
(cd "$ROOT" && "$ROOT/terrain/build/density_parity_test" "$SEED" "$OUT/cpp.txt" "$OUT/cpp.bin")
python3 "$SCRIPT_DIR/compare_density_parity.py" "$OUT/java.txt" "$OUT/java.bin" "$OUT/cpp.txt" "$OUT/cpp.bin"
