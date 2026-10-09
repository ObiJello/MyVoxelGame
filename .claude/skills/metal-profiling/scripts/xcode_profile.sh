#!/bin/bash
# Profile a .gputrace in Xcode at the MAXIMUM performance state and export
# its encoder counters: open, Replay (profile after replay), open the
# Performance view, switch the profiler session to Maximum, re-profile,
# export the encoder-counters CSV, screenshot the Overview.
#
#   xcode_profile.sh <capture.gputrace> <label>
#
# Writes $OUT_DIR/<label>_counters.csv and $OUT_DIR/<label>_overview.png.
# Coordinates are for the window placed at {0,30} size {1710,1070} on the
# Air's 1710x1107 desktop (Xcode 26.2); every click is a real CGEvent
# (scripts/mclick) because Xcode's SwiftUI tables ignore accessibility
# clicks. Keep the terminal out of the front while it runs.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
cap="$1"; label="$2"
out="${OUT_DIR:-${TMPDIR:-/tmp}}"
[ -x "$here/mclick" ] || xcrun swiftc -O -o "$here/mclick" "$here/mclick.swift"
click() { "$here/mclick" "$1" "$2"; sleep "${3:-1.5}"; }

osascript -e 'tell application "Xcode" to activate'; sleep 1
# Close every Xcode window already open (a second capture window would take
# the clicks). Capture windows report an EMPTY name through System Events,
# so nothing below selects a window by title: it is always "front window".
osascript -e 'tell application "System Events" to tell process "Xcode"
  repeat with w in windows
    try
      click button 1 of w
    end try
  end repeat
end tell' >/dev/null 2>&1 || true
sleep 1
open -a Xcode "$cap"; sleep 10
osascript -e 'tell application "Xcode" to activate'
osascript -e 'tell application "System Events" to tell process "Xcode"
  set position of front window to {0, 30}
  set size of front window to {1710, 1070}
end tell'; sleep 1
# The navigator's Performance row shows the profiled ms once profiling is
# done (the accessibility tree does not expose the "Profiling GPU Trace..."
# spinner reliably): poll a screenshot of that row until it changes from
# the state captured before the step began, with a ceiling.
row_md5() { screencapture -x -R 40,208,260,24 "$out/row_poll.png"; md5 -q "$out/row_poll.png"; }
wait_row_change() {   # <baseline md5> <max seconds>
  local base="$1" max="${2:-600}" i
  for i in $(seq 1 $((max / 5))); do
    sleep 5
    [ "$(row_md5)" != "$base" ] && return 0
  done
  echo "profiling step did not finish in $max s" >&2
}
# The replay screen: "Profile after replay" is on by default; Replay.
pos=$(osascript -e 'tell application "System Events" to tell process "Xcode"
  repeat with e in (entire contents of front window)
    try
      if role of e is "AXButton" and title of e is "Replay" then return (item 1 of (position of e)) & "," & (item 2 of (position of e))
    end try
  end repeat
end tell' 2>/dev/null | tr -d ' ')
row0=$(row_md5)
if [ -n "$pos" ]; then
  click "${pos%,*}" "${pos#*,}" 10
else
  # The accessibility tree often returns nothing for the replay screen.
  # The button sits at the bottom centre of the primary editor: x = 581
  # with the assistant editor open (the usual state), 659 without. Try the
  # first; if the sidebar did not change (the replay never started), the
  # second.
  click 581 1019 6
  [ "$(row_md5)" = "$row0" ] && click 659 1019 6
fi
# Xcode replays, then profiles ("Profiling GPU Trace..." on the Summary);
# the Performance row only accepts the clicks once that spinner is gone.
# Polled through accessibility (Xcode must stay frontmost).
# The Summary's Performance box reads "Profiling GPU Trace..." until the
# first profile is in; that box is what changes (the navigator row is bare
# until then too, but its md5 proved unreliable 2026-10-07).
# The spinner animates, so "changed" is not "done": done is a box that is
# STABLE across two polls and differs from what it showed before the
# replay (an indirect-command-buffer frame profiled for minutes on
# 2026-10-07 while the first version of this poll broke out at once).
box_md5() { screencapture -x -R 1180,360,340,40 "$out/box_poll.png"; md5 -q "$out/box_poll.png"; }
box0=$(box_md5); sleep 8
prev=$(box_md5)
for i in $(seq 1 180); do
  sleep 5
  cur=$(box_md5)
  [ "$cur" = "$prev" ] && [ "$cur" != "$box0" ] && break
  prev="$cur"
done; sleep 5
# Performance view (navigator row), then the session popover: clock icon,
# Performance State -> Maximum, Profile. The row keeps the Medium value
# during the re-profile; a Maximum value that happens to read the same
# falls through to the ceiling.
click 111 219 5
click 1640 135 2
click 1585 213 1.5
click 1557 237 1.5
row1=$(row_md5)
click 1519 429 10
wait_row_change "$row1" 150
sleep 5
# Counters tab -> share -> Export Encoder Counters -> save as <label>_counters.csv in $out.
click 1193 135 5
click 1664 135 1.5
click 1610 158 2.5
osascript -e "tell application \"System Events\" to tell process \"Xcode\"
  keystroke \"a\" using command down
  keystroke \"${label}_counters\"
  delay 0.4
  keystroke \"g\" using {command down, shift down}
  delay 1
  keystroke \"$out\"
  delay 0.5
  key code 36
  delay 1.5
  key code 36
end tell"; sleep 3
# Overview screenshot (encoder costs + top shaders).
click 823 135 3
screencapture -x "$out/${label}_overview.png"; sips -Z 1710 "$out/${label}_overview.png" >/dev/null
ls -la "$out/${label}_counters.csv" "$out/${label}_overview.png"
