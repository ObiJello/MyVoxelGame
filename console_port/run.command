#!/bin/bash
set -euo pipefail
PORT_ROOT="$(cd -- "$(dirname -- "$0")" && pwd)"
if ! command -v cmake >/dev/null 2>&1; then
    export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
fi
cmake -S "$PORT_ROOT" -B "$PORT_ROOT/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$PORT_ROOT/build" --target minecraft_console -j 6
exec "$PORT_ROOT/build/minecraft_console" --data-dir "$PORT_ROOT/saves" "$@"
