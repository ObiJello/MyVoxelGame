#!/usr/bin/env bash
# Builds the Tracy command-line tools at EXACTLY the version the game links —
# the tracy GIT_TAG in CMakeLists.txt is the single pin — into tools/tracy/
# (git-ignored):
#
#   tracy-export     tools/tracy_export — every table a capture holds, as CSV
#                    (what tools/tracy_report.py, analyze_trace.py and
#                    gpu_report.py read)
#   tracy-capture    headless capture:  tracy-capture -o out.tracy [-s seconds]
#   tracy-csvexport  Tracy's own CSV exporter (kept for ad-hoc use)
#   tracy-update     converts captures from older Tracy releases
#
#   tools/build_tracy_tools.sh           build if tools/tracy/VERSION != the pin
#   tools/build_tracy_tools.sh --force   rebuild anyway
#   tools/build_tracy_tools.sh --check   exit 0 if up to date, 1 if not
#
# Upgrading Tracy: bump GIT_TAG in CMakeLists.txt to the viewer's version,
# delete cmake-build-*/_deps/tracy-* (a stale libTracyClient survives the bump),
# then run this. The report scripts run it themselves when the pin moves.
#
# The Tracy checkout, its CPM dependencies and the build trees are cached in
# $OBEY_TRACY_CACHE (default ~/Library/Caches/obeycraft-tracy on macOS,
# ~/.cache/obeycraft-tracy elsewhere).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/tools/tracy"

TAG="$(awk '/GIT_REPOSITORY https:\/\/github.com\/wolfpld\/tracy/ { found = 1 }
            found && $1 == "GIT_TAG" { print $2; exit }' "$ROOT/CMakeLists.txt")"
if [[ -z "$TAG" ]]; then
    echo "build_tracy_tools: no tracy GIT_TAG found in CMakeLists.txt" >&2
    exit 1
fi
VERSION="${TAG#v}"

mode="build"
case "${1:-}" in
    --force) mode="force" ;;
    --check) mode="check" ;;
    "") ;;
    *) sed -n '2,24p' "$0" >&2; exit 1 ;;
esac

TOOLS=(tracy-export tracy-capture tracy-csvexport tracy-update)
up_to_date() {
    [[ -f "$OUT/VERSION" && "$(cat "$OUT/VERSION")" == "$TAG" ]] || return 1
    local t
    for t in "${TOOLS[@]}"; do [[ -x "$OUT/$t" ]] || return 1; done
    # tracy_export sources newer than the binary: rebuild the exporter.
    [[ -z "$(find "$ROOT/tools/tracy_export" -newer "$OUT/tracy-export" -type f)" ]]
}

if [[ "$mode" == "check" ]]; then
    if up_to_date; then echo "Tracy tools up to date ($TAG)"; exit 0; fi
    echo "Tracy tools stale: pin is $TAG, tools/tracy/VERSION is $(cat "$OUT/VERSION" 2>/dev/null || echo none)"
    exit 1
fi
if [[ "$mode" == "build" ]] && up_to_date; then
    echo "Tracy tools up to date ($TAG) in $OUT"
    exit 0
fi

for cmd in git cmake ninja; do
    command -v "$cmd" >/dev/null || { echo "build_tracy_tools: $cmd not found" >&2; exit 1; }
done

if [[ "$(uname)" == "Darwin" ]]; then
    CACHE="${OBEY_TRACY_CACHE:-$HOME/Library/Caches/obeycraft-tracy}"
else
    CACHE="${OBEY_TRACY_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/obeycraft-tracy}"
fi
SRC="$CACHE/tracy-$TAG"
BUILD="$CACHE/build-$TAG"
mkdir -p "$CACHE" "$OUT" "$BUILD/logs"

if [[ ! -f "$SRC/public/common/TracyVersion.hpp" ]]; then
    echo "==> Fetching Tracy $TAG"
    rm -rf "$SRC"
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$TAG" https://github.com/wolfpld/tracy.git "$SRC"
fi

JOBS="$( (sysctl -n hw.ncpu || nproc) 2>/dev/null || echo 4)"
configure_and_build() {   # <name> <source dir> [extra cmake args...]
    local name="$1" src="$2"; shift 2
    echo "==> Building $name"
    cmake -S "$src" -B "$BUILD/$name" -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DCPM_SOURCE_CACHE="$CACHE/cpm" "$@" > "$BUILD/logs/$name.configure.log" 2>&1 \
        || { cat "$BUILD/logs/$name.configure.log" >&2; exit 1; }
    cmake --build "$BUILD/$name" -j "$JOBS" > "$BUILD/logs/$name.build.log" 2>&1 \
        || { tail -40 "$BUILD/logs/$name.build.log" >&2; exit 1; }
}

configure_and_build export    "$ROOT/tools/tracy_export" -DTRACY_SRC="$SRC"
configure_and_build capture   "$SRC/capture"
configure_and_build csvexport "$SRC/csvexport"
configure_and_build update    "$SRC/update"

cp "$BUILD/export/tracy-export"       "$OUT/"
cp "$BUILD/capture/tracy-capture"     "$OUT/"
cp "$BUILD/csvexport/tracy-csvexport" "$OUT/"
cp "$BUILD/update/tracy-update"       "$OUT/"

# Every tool must report the pinned version, or the pin and the checkout
# disagree (a moved tag, a stale cache).
for t in "${TOOLS[@]}"; do
    # (capture / update print their version in the usage text -V triggers.)
    got="$("$OUT/$t" -V 2>&1 | grep -m1 '^tracy-' || true)"
    if [[ "$got" != *"$VERSION"* ]]; then
        echo "build_tracy_tools: $t reports '$got', expected $VERSION — delete $SRC and rerun" >&2
        exit 1
    fi
done
echo "$TAG" > "$OUT/VERSION"

echo "==> Tracy $VERSION tools in $OUT: ${TOOLS[*]}"
echo "    The viewer must be $VERSION too: https://github.com/wolfpld/tracy/releases/tag/$TAG"
