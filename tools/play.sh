#!/bin/bash
# Launch a CLion-built game bundle the way the launcher does (through
# LaunchServices, no debugger) and deal with macOS Game Mode:
#
#   tools/play.sh [tracy|debug|release] [--build] [--in-place] [--force-game-mode] [--gpu-trace[=SECONDS]] [--gpu-template=NAME|PATH] [game args...]
#
# --gpu-trace records the GPU side of the session with Instruments' Metal
# System Trace (xctrace, attached to the running game): per-frame GPU time,
# per-encoder vertex/fragment work, GPU utilisation, CPU/GPU pipelining. It
# starts recording 20 s after launch (world loaded, you at your spot) for
# SECONDS (default 45) and writes gpu-<time>.trace in the project root;
# tools/gpu_report.py reads it. A Tracy capture can run at the same time —
# they measure different things (CPU zones vs GPU timeline), and
# gpu_report.py --tracy aligns the two. --gpu-template swaps the Instruments
# template: the stock 'Metal System Trace' has no usable GPU counters on
# Apple silicon from the command line ("Selected counter profile is not
# supported"), so for limiter counters save a template from Instruments
# (Metal GPU Counters instrument, counter set chosen in its settings) and
# pass its .tracetemplate path here. Recording finishes AFTER the game
# quits: xctrace keeps running for a few minutes turning the raw capture
# into the document, so wait for "gpu trace saved" before reading it.
#   (--build runs `cmake --build cmake-build-<cfg> --target MyVoxelGame` first;
#    the CLion run configurations in .run/ use it, so one click builds + plays)
#   tools/play.sh tracy --vulkan                    # the profiling default
#   tools/play.sh tracy --force-game-mode --vulkan  # policy forced ON for the session
#
# --force-game-mode uses Xcode's gamepolicyctl to override the policy to
# "on" for the session (restored to "auto" when the game quits). That
# is the supported way to profile with Game Mode engaged whatever the
# automatic checks think of a dev build.
#
# Ten seconds after launch (the game is frontmost by then) it prints
# `gamepolicyctl game-mode status`, which under the automatic policy lists
# the requirement that is failing — that is the diagnostic for "why not".
#
# By default the bundle is COPIED to ~/Applications and that copy is run.
# This is what makes Game Mode work (verified in gamepolicyd's log,
# 2026-09-04): the daemon identifies a game by reading its Info.plist, it
# has no Full Disk Access, and this build tree sits under ~/Desktop, a
# folder macOS gates — so a bundle run from here is filed as "not a game"
# and never gets Game Mode, launcher-style launch or not. --in-place skips
# the copy (no Game Mode) if you need the exact build-tree path.
set -e
cfg="${1:-tracy}"; shift || true
install=1; force=0; build=0; gputrace=0; gputemplate='Metal System Trace'
while [ $# -gt 0 ]; do
    case "$1" in
        --build) build=1; shift ;;
        --gpu-trace) gputrace=45; shift ;;
        --gpu-trace=*) gputrace="${1#--gpu-trace=}"; shift ;;
        --gpu-template=*) gputemplate="${1#--gpu-template=}"; shift ;;
        --install) install=1; shift ;;
        --in-place) install=0; shift ;;
        --force-game-mode) force=1; shift ;;
        *) break ;;
    esac
done
root="$(cd "$(dirname "$0")/.." && pwd)"
if [ "$build" = 1 ]; then
    cmake --build "$root/cmake-build-$cfg" --target MyVoxelGame -j"$(sysctl -n hw.ncpu)"
fi
app="$root/cmake-build-$cfg/bin/MyVoxelGame.app"
[ -d "$app" ] || { echo "no bundle at $app — build the '$cfg' configuration in CLion first"; exit 1; }
if [ "$install" = 1 ]; then
    dest="$HOME/Applications/MyVoxelGame-$cfg.app"
    mkdir -p "$HOME/Applications"; rm -rf "$dest"; cp -R "$app" "$dest"; app="$dest"
    echo "copied to $app"
fi
GP="xcrun gamepolicyctl"
if [ "$force" = 1 ]; then
    $GP game-mode set on
    trap '$GP game-mode set auto; echo "(game mode policy restored to auto)"' EXIT
fi
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$app"
# `open -W` returns when the game quits, so the script — and CLion's Run
# window — finishes on its own; no Ctrl-C needed. Otherwise launched exactly
# as the launcher does it (no -n: a new-instance launch gets a throwaway
# identity that skips the daemon's game lookup).
open -W "$app" --args "$@" &
open_pid=$!
if [ "$gputrace" != 0 ]; then
    (
        sleep 20
        pid="$(pgrep -n -x MyVoxelGame || true)"
        if [ -z "$pid" ]; then echo "--- gpu trace: game process not found ---"; exit 0; fi
        out="$root/gpu-$(date +%H-%M-%S).trace"
        echo "--- gpu trace: recording Metal System Trace for ${gputrace}s -> $out ---"
        echo "--- gpu trace: KEEP THE GAME RUNNING until $(date -v+${gputrace}S +%H:%M:%S) (quitting earlier leaves an unreadable bundle) ---"
        xcrun xctrace record --template "$gputemplate" --attach "$pid" \
            --time-limit "${gputrace}s" --output "$out" >/dev/null 2>&1 \
            && echo "--- gpu trace saved: $out (tools/gpu_report.py $out) ---" || echo "--- gpu trace FAILED (is Xcode installed and licensed?) ---"
    ) &
fi
log="$HOME/Library/Application Support/obeycraft/logs/latest.log"
# The daemon's own verdict, sampled for a minute into a file: under the
# automatic policy the status names the requirement that is failing.
statusfile="$HOME/Library/Application Support/obeycraft/logs/gamemode-status.txt"
( for i in $(seq 1 12); do sleep 5; { date "+%H:%M:%S"; $GP game-mode status 2>&1 | sed 's/\x1b\[[0-9;]*m//g'; } >> "$statusfile"; done ) &
sampler_pid=$!
echo "--- game-mode status is being sampled into $statusfile for 60 s ---"
sleep 2
echo "--- tailing $log until the game quits ---"
tail -n 20 -f "$log" &
tail_pid=$!
wait "$open_pid" 2>/dev/null || true
kill "$tail_pid" "$sampler_pid" 2>/dev/null || true
echo "--- game quit ---"
