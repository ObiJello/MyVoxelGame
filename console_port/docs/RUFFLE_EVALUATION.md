# Original menu SWFs in Ruffle

On September 28, 2026, the original 720p menu movies were tested in a headless
Chrome session with the official `@ruffle-rs/ruffle` 0.6.0 web package. The HTTP
server supplied the archived files directly and resolved Iggy's library aliases:
`skin.swf`, `skinGraphics.swf`, and `platformskin.swf` (the latter to PS3
`skinPS3.swf`). This test did not launch a visible game or alter the archive.

Ruffle reads the movies' metadata (1280 x 720, 30 fps, SWF 9, ActionScript 3),
and loads `skinPS3.swf` without a loader error. `MainMenu720.swf`, however,
does not construct its buttons. Ruffle reports `Invalid PlaceObject type` for
seven class-only `PlaceObject3` tags, followed by an ActionScript null-reference
in `fourj.documents.MainMenu`. The original main menu uses class names such as
`fourj.Buttons.FJ_MenuButton_Normal` in these tags, with neither a character ID
nor a move flag. This is the SWF mechanism for placing symbols imported from
another SWF; it is not an archive corruption.

The static audit is reproducible without Ruffle:

```sh
python3 tools/audit_ruffle_swf.py
```

It finds 7 class-only placements in `MainMenu720.swf`, 2 in `Panorama720.swf`,
and 171 in the shared `skin.swf`. Across the 108 archived `*720.swf` movies in
`Common/Media`, 104 use this placement form (1,419 occurrences total).

An isolated experiment inserted known class IDs from `skin.swf` into a copy of
the seven main-menu placements. Ruffle then passed its tag parser, but rejected
the IDs as non-registered in the menu movie and still failed to construct the
buttons. The copy was removed after testing. The original source's
`ImportAssets2` tags name the libraries but import zero individual symbols;
Iggy's library loading supplies them across movies. A tag rewrite alone does
not reproduce this behavior in Ruffle.

Result: do not make Ruffle the runtime for the playable C++ menus yet. It is
useful for testing self-contained art and for investigating timelines, but
using it for the original menus needs work in both Ruffle's class-only
placement support and its cross-movie library resolution, plus a bridge to the
game's native Iggy calls and custom draw regions. Continue using the archived
SWFs and original UI C++ as the source of layout and timing data for the native
client. Keep direct playback as a separate research track until it can render
`MainMenu720.swf` correctly in a headless test.
