#!/bin/bash
# Run script for Minecraft Async Chunk Test
# Uses Minecraft's actual async pipeline (supplyAsync for NOISE)

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

# Read classpath
CLASSPATH=$(cat "$SCRIPT_DIR/minecraft_classpath.txt")
CLASSPATH="$CLASSPATH:$SCRIPT_DIR/java"

# Default arguments
RADIUS=""
CENTER_X=0
CENTER_Z=0
SINGLE_MODE=""
SINGLE_X=""
SINGLE_Z=""
OUTPUT=""
VERBOSE=""
PHASES="all"
SEED=""
DUMP_FULL=""
TRACE_FEATURES=""
TRACE_OUTPUT=""
TRACE_FEATURE_FILTER=""
TRACE_STRUCTURES=""
TRACE_WATCH=""
STRUCTURE_TRACE_OUTPUT=""
TRACE_PLACEMENTS=""
PLACEMENT_RADIUS=""
DUMP_BLOCK_ENTITIES=""
DIMENSION=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --radius)
            RADIUS="$2"
            shift 2
            ;;
        --single)
            SINGLE_MODE="true"
            SINGLE_X="$2"
            SINGLE_Z="$3"
            shift 3
            ;;
        --center)
            CENTER_X="$2"
            CENTER_Z="$3"
            shift 3
            ;;
        --output)
            OUTPUT="$2"
            shift 2
            ;;
        --quiet)
            VERBOSE="--quiet"
            shift
            ;;
        --phases)
            PHASES="$2"
            shift 2
            ;;
        --seed)
            SEED="--seed $2"
            shift 2
            ;;
        --dump-full)
            DUMP_FULL="--dump-full"
            shift
            ;;
        --trace-features)
            TRACE_FEATURES="--trace-features"
            shift
            ;;
        --trace-output)
            TRACE_OUTPUT="--trace-output $2"
            shift 2
            ;;
        --trace-feature-filter)
            TRACE_FEATURE_FILTER="--trace-feature-filter $2"
            shift 2
            ;;
        --trace-watch)
            TRACE_WATCH="--trace-watch $2"
            shift 2
            ;;
        --dimension)
            DIMENSION="--dimension $2"
            shift 2
            ;;
        --trace-structures)
            TRACE_STRUCTURES="--trace-structures"
            shift
            ;;
        --structure-trace-output)
            STRUCTURE_TRACE_OUTPUT="--structure-trace-output $2"
            shift 2
            ;;
        --trace-placements)
            TRACE_PLACEMENTS="--trace-placements $2"
            shift 2
            ;;
        --placement-radius)
            PLACEMENT_RADIUS="--placement-radius $2"
            shift 2
            ;;
        --dump-block-entities)
            DUMP_BLOCK_ENTITIES="--dump-block-entities"
            shift
            ;;
        --world-type)
            WORLD_TYPE="--world-type $2"
            shift 2
            ;;
        --flat-preset)
            FLAT_PRESET="--flat-preset $2"
            shift 2
            ;;
        --flat-layers)
            FLAT_LAYERS_VAL="$2"
            shift 2
            ;;
        --single-biome)
            SINGLE_BIOME="--single-biome $2"
            shift 2
            ;;
        *)
            echo "Unknown option: $1"
            echo ""
            echo "Usage:"
            echo "  $0 --radius <radius> [options]"
            echo "  $0 --single <chunkX> <chunkZ> [options]   (detailed per-block output)"
            echo ""
            echo "Options:"
            echo "  --radius <n>       Radius of chunks to generate"
            echo "  --single <x> <z>   Generate single chunk with detailed per-block output"
            echo "  --center <x> <z>   Center chunk position (default: 0 0)"
            echo "  --output <file>    Output file path"
            echo "  --phases <spec>    Phase configuration (default: all)"
            echo "  --seed <long>      World seed (default: 12345)"
            echo "  --quiet            Suppress progress output"
            echo "  --trace-features   Enable feature placement tracing"
            echo "  --trace-output <f> Output file for feature trace"
            echo "  --trace-feature-filter <name> Trace only the matching placed feature"
            echo "  --trace-structures Dump structure references and starts affecting the generated chunks"
            echo "  --structure-trace-output <f> Output file for structure trace"
            exit 1
            ;;
    esac
done

# Validate arguments
if [ -z "$SINGLE_MODE" ] && [ -z "$RADIUS" ]; then
    echo "Error: Must specify either --radius or --single"
    echo ""
    echo "Usage:"
    echo "  $0 --radius <radius> [options]"
    echo "  $0 --single <chunkX> <chunkZ> [options]   (detailed per-block output)"
    exit 1
fi

# Set default output path
if [ -z "$OUTPUT" ]; then
    if [ -n "$SINGLE_MODE" ]; then
        OUTPUT="$SCRIPT_DIR/../output/java_single_${SINGLE_X}_${SINGLE_Z}.txt"
    else
        OUTPUT="$SCRIPT_DIR/../output/java_async_minecraft.txt"
    fi
fi

# Create output directory if needed
mkdir -p "$(dirname "$OUTPUT")"

echo "Running Minecraft Async Chunk Test..."
echo "Phases: $PHASES"
if [ -n "$SINGLE_MODE" ]; then
    echo "Mode: SINGLE CHUNK (detailed per-block output)"
    echo "Chunk: ($SINGLE_X, $SINGLE_Z)"
else
    echo "Radius: $RADIUS"
    echo "Center: ($CENTER_X, $CENTER_Z)"
fi
echo "Output: $OUTPUT"
echo ""

# Build Java arguments
JAVA_ARGS="--phases $PHASES --output $OUTPUT $SEED $DUMP_FULL $VERBOSE $TRACE_FEATURES $TRACE_OUTPUT $TRACE_FEATURE_FILTER $TRACE_WATCH $TRACE_STRUCTURES $STRUCTURE_TRACE_OUTPUT $TRACE_PLACEMENTS $PLACEMENT_RADIUS $DUMP_BLOCK_ENTITIES $DIMENSION ${WORLD_TYPE:-} ${FLAT_PRESET:-} ${SINGLE_BIOME:-}"
if [ -n "$SINGLE_MODE" ]; then
    JAVA_ARGS="$JAVA_ARGS --single $SINGLE_X $SINGLE_Z"
else
    JAVA_ARGS="$JAVA_ARGS --radius $RADIUS --center $CENTER_X $CENTER_Z"
fi

# Run with proper Java settings
# -XstartOnFirstThread is required on macOS for Minecraft
# --enable-native-access=ALL-UNNAMED is required for newer Java
time java \
    -XstartOnFirstThread \
    --enable-native-access=ALL-UNNAMED \
    --sun-misc-unsafe-memory-access=allow \
    -Xmx4G \
    -cp "$CLASSPATH" \
    MinecraftAsyncChunkTest \
    $JAVA_ARGS ${FLAT_LAYERS_VAL:+--flat-layers "$FLAT_LAYERS_VAL"}
