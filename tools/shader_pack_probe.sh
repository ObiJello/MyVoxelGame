#!/bin/zsh
# Shader-pack debugging runs on any backend, captured by window id (no focus needed).
#
#   tools/shader_pack_probe.sh <gl|metal|vulkan> show <buffer[@stage]> <out.png>
#       OBEY_PACK_SHOW: the final pass shows <buffer> — colortex0..7, depthtex0/1/2,
#       shadowtex0, shadowcolor0, noisetex, lightmap; @gbuffers = as the gbuffers and
#       deferred passes left it (composites skipped), @deferred = the snapshot taken
#       before the translucent pass (Vulkan/Metal).
#   tools/shader_pack_probe.sh <backend> probe <program> "<glsl>" <out.png> [buffer[@stage]]
#       OBEY_PACK_PROBE: <program>'s first fragment output becomes vec4(<glsl>) — any
#       global of the pack's fragment stage (varyings, uniforms, samplers, functions),
#       e.g. gbuffers_water "texture2D(colortex4, gl_FragCoord.xy / vec2(viewWidth, viewHeight)).rgb, 1.0".
#   Extra engine switches through EXTRA_ENV="--env NAME=VALUE ...".
#
# The run joins "OG with Structs" (WORLD=... overrides), captures at t≈9 s and keeps
# the run's log next to the png. OBEY_PACK_DUMP=1 writes the translated GLSL (and
# with =msl the Metal source) to <obeycraft>/shaderpacks/.translated/.
set -e
root="$(cd "$(dirname "$0")/.." && pwd)"
backend="$1"; mode="$2"; shift 2
flag="--$backend"; [ "$backend" = "gl" ] && flag=""
case "$mode" in
    show)  show="$1"; out="$2"; probe="" ;;
    probe) program="$1"; expr="$2"; out="$3"; show="${4:-colortex4@gbuffers}"; probe="--env OBEY_PACK_PROBE=$program:$expr" ;;
    *) echo "usage: $0 <gl|metal|vulkan> show <buffer> <out.png> | probe <program> <glsl> <out.png> [buffer]"; exit 1 ;;
esac
L="$HOME/Library/Application Support/obeycraft/logs/latest.log"
winid="$root/tools/winid"
if [ ! -x "$winid" ]; then
    cat > /tmp/winid.swift <<'SWIFT'
import Cocoa
guard let list = CGWindowListCopyWindowInfo([.optionOnScreenOnly], kCGNullWindowID) as? [[String: Any]] else { exit(1) }
var best: (Int, Double)? = nil
for w in list {
    let owner = w[kCGWindowOwnerName as String] as? String ?? ""
    guard owner.contains("MyVoxelGame") else { continue }
    let b = w[kCGWindowBounds as String] as? [String: Double] ?? [:]
    let h = b["Height"] ?? 0
    if h > 400, let id = w[kCGWindowNumber as String] as? Int { if best == nil || h > best!.1 { best = (id, h) } }
}
guard let (id, _) = best else { exit(1) }
print(id)
SWIFT
    swiftc -O -o "$winid" /tmp/winid.swift
fi
rm -f "$L"
( "$root/tools/play.sh" tracy $flag --world "${WORLD:-OG with Structs}" --quit-after 22 --env "OBEY_PACK_SHOW=$show" ${probe:+"$probe"} ${=EXTRA_ENV} > /dev/null 2>&1 & )
sleep 10
for i in {1..40}; do grep -q "\[Harness\] t=9s" "$L" 2>/dev/null && break; sleep 1; done
id=$("$winid") && screencapture -x -o -l "$id" "$out"
for i in {1..30}; do grep -q "log closed cleanly" "$L" 2>/dev/null && break; sleep 1; done
cp "$L" "$out.log" 2>/dev/null || true
echo "$out"
