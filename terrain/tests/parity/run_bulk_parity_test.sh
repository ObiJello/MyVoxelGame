#!/bin/bash

# Bulk Parity Test Runner
# Runs both Java (serverless, phases 3-6) and C++ for a 100x100 chunk grid and
# compares FULL per-chunk block histograms (every block type + count must
# match, not just air counts). Histogram entry order is ignored — both sides
# sort ties differently — so the comparison parses entries into maps.
#
# Usage: ./run_bulk_parity_test.sh [seed]      (default 12345)
# Exit codes: 0 = all chunks match, 1 = mismatches or structural error.

set -e

SEED="${1:-12345}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
OUTPUT_DIR="$SCRIPT_DIR/../output"

mkdir -p "$OUTPUT_DIR"

JAVA_OUTPUT="$OUTPUT_DIR/java_bulk_100x100_s$SEED.txt"
CPP_OUTPUT="$OUTPUT_DIR/cpp_bulk_100x100_s$SEED.txt"

echo "=== Bulk Parity Test Runner (seed $SEED) ==="

# C++ binary comes from the cmake target
CPP_TEST="$PROJECT_DIR/build/bulk_parity_test"
if [ ! -f "$CPP_TEST" ]; then
    echo "Building bulk_parity_test..."
    (cd "$PROJECT_DIR/build" && make bulk_parity_test -j8)
fi

echo ""
echo "=== Running C++ Bulk Test ==="
cd "$PROJECT_DIR"
"$CPP_TEST" "$CPP_OUTPUT" "$SEED"

# Set up Java
MC_VERSION="26.1-snapshot-1"
MC_JAR="$HOME/Library/Application Support/minecraft/versions/$MC_VERSION/$MC_VERSION.jar"

if [ ! -f "$MC_JAR" ]; then
    echo "ERROR: Minecraft jar not found at: $MC_JAR"
    exit 1
fi

# Classpath (cached list built by build_minecraft_async_test.sh)
if [ -f "$SCRIPT_DIR/minecraft_classpath.txt" ]; then
    CLASSPATH="$(cat "$SCRIPT_DIR/minecraft_classpath.txt")"
else
    LIBRARIES_DIR="$HOME/Library/Application Support/minecraft/libraries"
    CLASSPATH="$MC_JAR"
    while IFS= read -r -d '' jar; do
        CLASSPATH="$CLASSPATH:$jar"
    done < <(find "$LIBRARIES_DIR" -name "*.jar" -print0)
fi
CLASSPATH="$CLASSPATH:$SCRIPT_DIR/java"

JAVA_25=$(/usr/libexec/java_home -v 25 2>/dev/null || echo "")
if [ -z "$JAVA_25" ]; then
    echo "ERROR: Java 25 is required for Minecraft $MC_VERSION"
    exit 1
fi
export JAVA_HOME="$JAVA_25"

echo ""
echo "=== Compiling Java Bulk Test ==="
"$JAVA_HOME/bin/javac" -cp "$CLASSPATH" -d "$SCRIPT_DIR/java/" "$SCRIPT_DIR/java/MinecraftBulkTest.java"

echo ""
echo "=== Running Java Bulk Test ==="
JVM_ARGS="-Xmx4G"
JVM_ARGS="$JVM_ARGS --add-opens java.base/java.lang=ALL-UNNAMED"
JVM_ARGS="$JVM_ARGS --add-opens java.base/java.util=ALL-UNNAMED"

(cd "$SCRIPT_DIR" && "$JAVA_HOME/bin/java" $JVM_ARGS -cp "$CLASSPATH" MinecraftBulkTest "$JAVA_OUTPUT" "$SEED")

echo ""
echo "=== Comparing full per-chunk histograms ==="

set +e
python3 - "$JAVA_OUTPUT" "$CPP_OUTPUT" <<'EOF'
import sys

def load(path):
    chunks = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(",")
            cx, cz = int(parts[0]), int(parts[1])
            air, non_air = int(parts[2]), int(parts[3])
            hist = {}
            for entry in parts[4:]:
                name, _, count = entry.rpartition(":")
                hist[name] = int(count)
            chunks[(cx, cz)] = (air, non_air, hist)
    return chunks

java = load(sys.argv[1])
cpp = load(sys.argv[2])

bad = 0
if set(java) != set(cpp):
    only_j = sorted(set(java) - set(cpp))
    only_c = sorted(set(cpp) - set(java))
    print(f"STRUCTURAL: chunk sets differ (java-only {len(only_j)}, cpp-only {len(only_c)})")
    for c in (only_j + only_c)[:10]:
        print(f"  {c}")
    sys.exit(1)

for key in sorted(java):
    ja, jn, jh = java[key]
    ca, cn, ch = cpp[key]
    if (ja, jn) != (ca, cn) or jh != ch:
        bad += 1
        if bad <= 10:
            print(f"MISMATCH chunk {key}: air {ja}/{ca} nonair {jn}/{cn}")
            for name in sorted(set(jh) | set(ch)):
                if jh.get(name, 0) != ch.get(name, 0):
                    print(f"    {name}: java={jh.get(name, 0)} cpp={ch.get(name, 0)}")

print(f"Chunks compared: {len(java)}")
if bad:
    print(f"MISMATCH: {bad} chunks differ in their full block histogram")
    sys.exit(1)
print("SUCCESS: all per-chunk block histograms identical.")
EOF
rc=$?

echo ""
echo "=== Test Complete (seed $SEED, exit $rc) ==="
exit $rc
