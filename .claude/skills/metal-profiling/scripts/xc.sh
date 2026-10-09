#!/bin/bash
# Drive Xcode's GPU-capture window with REAL mouse events and take a
# screenshot. SwiftUI tables ignore AppleScript/accessibility clicks, so the
# clicks come from scripts/mclick (CGEvent), built here on first use.
#
#   xc.sh out.png [click X Y | dbl X Y | key '<applescript on process Xcode>' | wait SEC]...
#
# Coordinates are logical screen points (the screenshot is scaled to 1710 px
# wide, which on the Air's 1710x1107 desktop is 1:1). Keep the terminal out of
# the front: the script activates Xcode first and every click lands on it.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
out="${OUT_DIR:-${TMPDIR:-/tmp}}"
[ -x "$here/mclick" ] || xcrun swiftc -O -o "$here/mclick" "$here/mclick.swift"
shot="$1"; shift
osascript -e 'tell application "Xcode" to activate' >/dev/null; sleep 1
while [ $# -gt 0 ]; do
    case "$1" in
        click) "$here/mclick" "$2" "$3"; shift 3; sleep 1.5 ;;
        dbl)   "$here/mclick" "$2" "$3" 2; shift 3; sleep 2 ;;
        move)  "$here/mclick" "$2" "$3" 0; shift 3; sleep 1.5 ;;
        key)   osascript -e "tell application \"System Events\" to tell process \"Xcode\" to $2" >/dev/null 2>&1; shift 2; sleep 1 ;;
        wait)  sleep "$2"; shift 2 ;;
        *) echo "unknown op: $1" >&2; exit 2 ;;
    esac
done
screencapture -x "$out/$shot" && sips -Z 1710 "$out/$shot" >/dev/null
echo "$out/$shot"
