#!/bin/bash
# Launch a CLion-built game bundle the way the launcher does (through
# LaunchServices, no debugger) and deal with macOS Game Mode:
#
#   tools/play.sh [tracy|debug|release] [--build] [--in-place] [--force-game-mode] [--gpu-trace[=SECONDS]] [--gpu-template=NAME|PATH] [--hud] [game args...]
#
# --gpu-trace records the GPU side of the session with Instruments (xctrace,
# ATTACHED to the running game — never --launch: xctrace resolves the app
# through LaunchServices by name and started the INSTALLED game even when
# given this copy's executable path and a distinct bundle id, 2026-10-06;
# the copy made here still gets its own bundle id, see below): per-frame
# GPU time, per-encoder vertex/fragment work, GPU utilisation, CPU/GPU
# pipelining. It starts recording 20 s after launch (world loaded, you at
# your spot) for SECONDS (default 45) and writes gpu-<time>.trace in the
# project root; tools/gpu_report.py reads it. A Tracy capture can run at the
# same time — they measure different things (CPU zones vs GPU timeline),
# and gpu_report.py --tracy aligns the two. --gpu-template swaps the
# Instruments template: the stock 'Metal System Trace' records no useful
# GPU counters; gpu-limiters.tracetemplate (project root, saved from the
# Instruments GUI: counter set "Performance Limiters", the only live set
# this M4 offers, shader timeline OFF) adds the 13 limiter counters — and
# ONLY works attached, which is what this script does: every all-process or
# launched recording with that counter set crashed Instruments' GPUPlugin
# (xctrace four times, Instruments.app once, Xcode 26.2, 2026-10-06).
# gpu-shaders.tracetemplate (counter set None, shader timeline ON) records
# without crashing in any mode but has produced no shader-timeline samples
# in attached, all-process or launched recordings so far. Recording
# finishes AFTER the game quits: xctrace keeps running for a few minutes
# turning the raw capture into the document, so wait for "gpu trace saved"
# before reading it; quitting the game before the time limit elapses leaves
# an unreadable trace. xctrace's own output goes to <trace>.log.
#
# --hud shows Apple's Metal Performance HUD with per-ENCODER GPU timing
# (overlay only) and captures its per-frame log — `metal-HUD:` NSLog lines
# on the process's stderr, which `open --stderr` redirects into
# logs/hud-<time>.log (.claude/skills/metal-profiling/scripts/hud_log.py
# summarises it). The encoder timing is blank while the game attaches
# counter sample buffers, so do not combine it with OBEY_MTL_GPU_TIMERS. The
# HUD counts PRESENTED frames: with vsync off the mailbox renders more
# frames than it shows.
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
# 2026-09-04, when the tree sat under ~/Desktop): the daemon identifies a
# game by reading its Info.plist and has no Full Disk Access, so a bundle in
# a TCC-gated folder is filed as "not a game" and never gets Game Mode. The
# tree now lives in ~/Developer; the copy stays the proven path, and it is
# also where the copy gets its own CFBundleIdentifier
# (com.yourcompany.MyVoxelGame.<cfg>, re-signed ad hoc) so Instruments,
# `open` and LaunchServices can tell it from the installed game. --in-place
# skips the copy (no Game Mode) if you need the exact build-tree path.
set -e
cfg="${1:-tracy}"; shift || true
install=1; force=0; build=0; gputrace=0; gputemplate='Metal System Trace'; hud=0
while [ $# -gt 0 ]; do
    case "$1" in
        --build) build=1; shift ;;
        --gpu-trace) gputrace=45; shift ;;
        --gpu-trace=*) gputrace="${1#--gpu-trace=}"; shift ;;
        --gpu-template=*) gputemplate="${1#--gpu-template=}"; shift ;;
        --hud) hud=1; shift ;;
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
    # Its own identity: the signature binds Info.plist, so re-sign after the
    # edit (ad hoc, as the build signs it).
    /usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier com.yourcompany.MyVoxelGame.$cfg" "$app/Contents/Info.plist"
    codesign --force --sign - "$app" >/dev/null 2>&1 || echo "(re-sign failed; the copy keeps the build's signature)"
    echo "copied to $app (bundle id com.yourcompany.MyVoxelGame.$cfg)"
fi
exe="$app/Contents/MacOS/MyVoxelGame"
env_args=()
if [ "$hud" = 1 ]; then
    hudlog="$HOME/Library/Application Support/obeycraft/logs/hud-$(date +%H-%M-%S).log"
    env_args+=(--env MTL_HUD_ENABLED=1 --env MTL_HUD_ENCODER_TIMING_ENABLED=1 --env MTL_HUD_LOG_ENABLED=1
               --env MTL_HUD_ALIGNMENT=bottomleft
               --env MTL_HUD_ELEMENTS=fps,gputime,frameinterval,toplabeledencoders,gputimeline,shaders,metalcpu,memory
               --stderr "$hudlog")
    echo "--- Metal Performance HUD on; its per-frame log goes to $hudlog (hud_log.py summarises it) ---"
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
open -W "${env_args[@]}" "$app" --args "$@" &
open_pid=$!
if [ "$gputrace" != 0 ]; then
    (
        sleep 20
        # The process of THIS bundle, not any MyVoxelGame that happens to run
        # (the installed game, another config's copy).
        pid="$(pgrep -n -f "^$exe" || true)"
        if [ -z "$pid" ]; then echo "--- gpu trace: game process not found ($exe) ---"; exit 0; fi
        out="$root/gpu-$(date +%H-%M-%S).trace"
        echo "--- gpu trace: recording '$gputemplate' for ${gputrace}s -> $out ---"
        echo "--- gpu trace: KEEP THE GAME RUNNING until $(date -v+${gputrace}S +%H:%M:%S) (quitting earlier leaves an unreadable bundle) ---"
        # xctrace records into a raw instruments*.ktrace in $TMPDIR and never
        # deletes it (0.5-2 GB per recording — 36 GB of them filled the disk
        # on 2026-09-25). The ones this recording made go once it is done.
        marker="$(mktemp -t gputrace-start)"
        xcrun xctrace record --template "$gputemplate" --attach "$pid" \
            --time-limit "${gputrace}s" --output "$out" >"$out.log" 2>&1 \
            && echo "--- gpu trace saved: $out (tools/gpu_report.py $out) ---" \
            || echo "--- gpu trace FAILED (exit $?; see $out.log — counter templates work attached only; they crash GPUPlugin on all-process/launched recordings, Xcode 26.2) ---"
        find "${TMPDIR:-/tmp}" -maxdepth 1 -name 'instruments*.ktrace' -newer "$marker" -delete 2>/dev/null
        rm -f "$marker"
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
