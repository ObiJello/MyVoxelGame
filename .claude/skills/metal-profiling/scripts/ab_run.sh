#!/bin/bash
# One measured Metal run of the `tour` replay for an A/B: Game Mode copy,
# full screen, clear weather (RAIN=1 for rain), RD 32, vsync off, the engine's per-encoder GPU timers
# on, a Tracy capture, and optionally an Xcode GPU capture of frame ~45 s.
#
#   [APP=<bundle>] [BACKEND=--vulkan] [RAIN=1] [RAINHALF=1] [STILL=1] [SCALE=50] [RD=12] [REPLAY=tour1] [FOG=0] [WORLDLIGHT=0] ab_run.sh <label> [--capture] [--env NAME=VALUE ...]
#
# Writes <out>/<label>.tracy (and the .gputrace under obeycraft/captures)
# and prints the Tracy frame stats + Gpu/* plot means. OUT_DIR defaults to
# the scratchpad. The hold (8 s parked on the first pose while the chunks
# stream in) plus the 57.4 s path must fit inside --quit-after, or the run
# is a still frame at the first pose and never plays the recording — check
# the log for "[Replay] t=+..s playing" lines. The capture frame at 45 s is
# then t=37 s of the path, the same frame every run. Pair runs ABBA (A B B A) on this fanless Air: it loses
# ~5 % a slot as it heats. Compare the Gpu/Enc*Us plots for GPU-side changes
# (overlapped, live) and the .gputrace at the Maximum performance state for
# the deterministic per-encoder ms of the same frame.
set -e
root="$(cd "$(dirname "$0")/../../../.." && pwd)"
label="$1"; shift
out="${OUT_DIR:-${TMPDIR:-/tmp}}"
capture=0; envs=()
while [ $# -gt 0 ]; do
    case "$1" in
        --capture) capture=1; shift ;;
        --env) envs+=(--env "$2"); shift 2 ;;
        *) echo "unknown arg $1" >&2; exit 2 ;;
    esac
done
# The measurement settings, forced for the run and the user's file put
# back whole afterwards: RD 32, vsync OFF (on, the run is capped at 60 fps
# and the mailbox is off — the user's in-game toggles between two series
# did exactly that on 2026-10-07), fog ON (MC's default load). Anything
# else is the user's current setting, so a series is only comparable with
# itself: the backup in $out says what the run saw.
opts="$HOME/Library/Application Support/obeycraft/options.txt"
cp "$opts" "$out/options.backup.txt"
trap 'cp "$out/options.backup.txt" "$opts"' EXIT
sed -i '' -e 's/^renderDistance:.*/renderDistance:32/' -e 's/^enableVsync:.*/enableVsync:false/' \
          -e "s/^fogEnabled:.*/fogEnabled:$([ "${FOG:-1}" = 0 ] && echo false || echo true)/" "$opts"
grep -q "^enableVsync:" "$opts" || echo "enableVsync:false" >> "$opts"
grep -q "^fogEnabled:" "$opts" || echo "fogEnabled:$([ "${FOG:-1}" = 0 ] && echo false || echo true)" >> "$opts"
# FOG=0: fog off for the run (the terrain shaders' fog skip; A/B it with
# --env OBEY_SKIP=fogskip). WORLDLIGHT=0: World Lighting off (the lightmap
# skip; --env OBEY_SKIP=lightskip). Both default to the measurement
# settings: fog on, lighting on.
if [ "${WORLDLIGHT:-1}" = 0 ]; then
    sed -i '' -e 's/^worldLighting:.*/worldLighting:false/' "$opts"
    grep -q "^worldLighting:" "$opts" || echo "worldLighting:false" >> "$opts"
else
    sed -i '' -e 's/^worldLighting:.*/worldLighting:true/' "$opts"
fi
# SCALE=<percent>: Render Resolution for the run (default: the user's). 50
# makes the frame GPU-light, i.e. CPU-bound — the regime where a command
# path's CPU cost (Metal 4 vs 3) can show; 100 is the GPU-bound one.
if [ -n "${SCALE:-}" ]; then
    sed -i '' -e "s/^renderScale:.*/renderScale:${SCALE}/" "$opts"
    grep -q "^renderScale:" "$opts" || echo "renderScale:${SCALE}" >> "$opts"
fi
# RAINHALF=1: Rain Resolution Half in the options for the run (with RAIN=1
# and --env OBEY_RAIN_HALF=1 on Metal, where Half is otherwise hidden).
if [ "${RAINHALF:-0}" = 1 ]; then
    sed -i '' -e 's/^rainHalfResolution:.*/rainHalfResolution:true/' "$opts"
    grep -q "^rainHalfResolution:" "$opts" || echo "rainHalfResolution:true" >> "$opts"
fi
osascript "$(dirname "$0")/close_notifs.applescript" >/dev/null 2>&1 || true
# The previous run's process must be gone: launched while one is still
# shutting down, macOS activates that instance instead of starting a new
# one and the capture waits forever (a series stalled on it, 2026-10-08).
for i in $(seq 1 40); do pgrep -x MyVoxelGame >/dev/null || break; sleep 0.5; done
rm -f "$out/$label.tracy"
"$root/tools/tracy/tracy-capture" -o "$out/$label.tracy" -f > "$out/$label.capture.log" 2>&1 &
sleep 1
# BACKEND=--vulkan (or --metal, the default): the same run on another
# backend, for the cross-backend A/Bs. The Gpu/* plots are Metal's; on
# Vulkan read Mtl-less rows (the frame stats, Draws/*, Geom/*).
# Weather: clear by default (user, 2026-10-07: measure with rain off; the
# Tier 1-5 series ran in rain). RAIN=1 forces rain for a rain-path test.
weather="/weather clear 1d"; [ "${RAIN:-0}" = 1 ] && weather="/weather rain 1d"
# STILL=1: a still frame at the tour's first pose for the whole run (hold
# longer than the quit) — no scene variation, so a phase A/B's paired
# interval is the machine's noise alone (tour: ±10 %; still: see the
# report). Use the tour for the realistic number, the still for a verdict.
hold=8; quit=70; [ "${STILL:-0}" = 1 ] && { hold=70; quit=60; }
# REPLAY=<name>: another recording than the tour; RD=<chunks>: another
# render distance than 32 (the hold/quit stay: a longer path is cut off).
replay="${REPLAY:-tour}"
sed -i '' -e "s/^renderDistance:.*/renderDistance:${RD:-32}/" "$opts"
launch=(${BACKEND:---metal} --world "OG with Structs" --replay "$replay" --replay-hold $hold --quit-after $quit
        --exec-at 1 "${weather}" --env OBEY_MTL_GPU_TIMERS=1 "${envs[@]}")
# APP=<bundle>: run that bundle as it is (a kept copy of another build, for
# an A/B across builds — play.sh would overwrite ~/Applications/
# A loaded machine (a Zoom call, an IDE indexing after edits) slows every
# main-thread zone together and the run reads as a regression: wait for the
# 1-minute load average to settle first (up to 5 min), 2026-10-08.
for _i in $(seq 1 20); do
    _load=$(sysctl -n vm.loadavg | awk '{print $2}')
    if [ "$(echo "$_load < 1.8" | bc)" = 1 ]; then break; fi
    [ "$_i" = 1 ] && echo "[ab_run] load average $_load — waiting for the machine to settle" >&2
    sleep 15
done
# MyVoxelGame-tracy.app with the current build). Register it so Game Mode
# and the capture env work the same way.
app="${APP:-$HOME/Applications/MyVoxelGame-tracy.app}"
if [ "$capture" = 1 ] || [ -n "${APP:-}" ]; then
    [ "$capture" = 1 ] && launch+=(--env OBEY_MTL_CAPTURE_AT=45)
    /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$app" >/dev/null 2>&1
    if [ "$capture" = 1 ]; then
        open -W --env MTL_CAPTURE_ENABLED=1 "$app" --args "${launch[@]}" > "$out/$label.play.log" 2>&1 &
    else
        open -W "$app" --args "${launch[@]}" > "$out/$label.play.log" 2>&1 &
    fi
else
    "$root/tools/play.sh" tracy "${launch[@]}" > "$out/$label.play.log" 2>&1 &
fi
sleep 12; osascript "$(dirname "$0")/close_notifs.applescript" >/dev/null 2>&1 || true
wait
sleep 2
log="$HOME/Library/Application Support/obeycraft/logs/latest.log"
grep -E "\[Window\] framebuffer|GPU timers on|capturing frame" "$log" | head -3 | sed "s/^/[$label] /"
python3 "$root/tools/tracy_report.py" "$out/$label.tracy" 2>/dev/null | grep -E " fps|frame time ms|frames >8|Gpu/Enc(Frame|Targets|Blits)Us|Gpu/Rain|Mtl/GpuFrameUs|Mtl/Draws |Mtl/FrameWaitUs|Mtl/PaceWaitUs|Mtl/GpuLatencyUs|Mtl/DeviceAllocMB|Geom/(Vertices|Indices) |Draws/(Merged|Entries) " | sed "s/^/[$label] /"
