#!/bin/bash
# Multiplayer stress test on one machine: a headless server for a world, then
# a bot swarm against it, both from the same build.
#
#   tools/stress_test.sh <world name> [release|debug|tracy] [bot args...]
#
#   tools/stress_test.sh "Stress World"                                   # 20 bots, spread, 120 s
#   tools/stress_test.sh "Stress World" release --bots 100 --bot-scenario fly --quit-after 300
#   tools/stress_test.sh "Stress World" release --bots 50 --bot-join-interval 0   # join storm
#   tools/stress_test.sh "Stress World" release --bots 40 --bot-scenario scatter --bot-hop 30   # random spots, all dimensions
#   BOTS_CFG=release tools/stress_test.sh "Stress World" tracy --bots 40 ...    # profile the server with Tracy
#
# Server and bots share this machine's CPU, so a server number here is a
# floor — for honest numbers run the swarm on another computer instead:
#   server:  MyVoxelGame --headless-server --world "<name>" [--port 25565]
#   bots:    MyVoxelGame --bots 50 --server <server-ip>:25565
#
# Reports (one line per second in each log, CSV beside it):
#   <obeycraft>/logs/server/latest.log   [ServerStats]  tps, mspt, phases, per-player send backlog
#   <obeycraft>/logs/bots/latest.log     [Bots]         chunks/s per bot, command round trip, ping
# The world is a real save and is written to — use a throwaway world.
set -e
world="$1"; shift || true
[ -n "$world" ] || { sed -n '2,20p' "$0"; exit 1; }
cfg=release
case "$1" in release|debug|tracy) cfg="$1"; shift ;; esac

root="$(cd "$(dirname "$0")/.." && pwd)"
binfor() {
    local b="$root/cmake-build-$1/bin/MyVoxelGame.app/Contents/MacOS/MyVoxelGame"
    [ -x "$b" ] || b="$root/cmake-build-$1/bin/MyVoxelGame"
    [ -x "$b" ] || { echo "no binary for '$1' — build it first" >&2; exit 1; }
    echo "$b"
}
bin="$(binfor "$cfg")"
# The bots can come from another build: profiling the server with Tracy
# (cfg=tracy) wants the bots in Release, or both processes feed the profiler.
botbin="$(binfor "${BOTS_CFG:-$cfg}")"

port="${STRESS_PORT:-25599}"   # beside a normal game on 25565
logs="$HOME/Library/Application Support/obeycraft/logs"

# Scatter teleports bots by command: the server must grant guest commands
# (saved with the world — another reason to use a throwaway one).
serverextra=()
case " $* " in *" scatter "*) serverextra=(--guest-commands) ;; esac

"$bin" --headless-server --world "$world" --port "$port" "${serverextra[@]}" &
server=$!
trap 'kill -INT $server 2>/dev/null; wait $server 2>/dev/null' EXIT

# Wait for the port (world load can take a few seconds).
for _ in $(seq 1 120); do
    if nc -z 127.0.0.1 "$port" 2>/dev/null; then break; fi
    kill -0 "$server" 2>/dev/null || { echo "server exited — see $logs/server/latest.log"; exit 1; }
    sleep 0.5
done
nc -z 127.0.0.1 "$port" || { echo "server never opened port $port"; exit 1; }

if [ $# -eq 0 ]; then set -- --bots 20 --quit-after 120; fi
case " $* " in *" --bots "*) ;; *) set -- --bots 20 "$@" ;; esac
case " $* " in *" --quit-after "*) ;; *) set -- "$@" --quit-after 120 ;; esac
"$botbin" --server "127.0.0.1:$port" "$@" || true

echo
echo "== server (last 5 s) =="
grep "\[ServerStats\] tps" "$logs/server/latest.log" | tail -5 || true
echo "== bots (last 5 s) =="
grep "\[Bots\] t=" "$logs/bots/latest.log" | tail -5 || true
grep "\[Bots\] summary" "$logs/bots/latest.log" || true
echo "CSVs: $logs/server/ and $logs/bots/"
