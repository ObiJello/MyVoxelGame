#!/usr/bin/env python3
"""
Generate a .DS_Store file for the ObeyCraft Launcher DMG.

This sets up the Finder window layout so when users open the DMG they see:
  - Icon view with large icons (128px)
  - ObeyCraftLauncher.app on the left
  - Applications shortcut on the right
  - Dark background, compact window

Run once (or whenever you want to change the layout):
    pip3 install ds_store
    python3 tools/generate_dmg_ds_store.py

Output: tools/dmg_ds_store (binary file, commit to repo)
"""

import struct
import os
from ds_store import DSStore

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUTPUT_PATH = os.path.join(SCRIPT_DIR, "dmg_ds_store")

# Window dimensions
WIN_WIDTH = 540
WIN_HEIGHT = 400
WIN_X = 100
WIN_Y = 100

# Icon positions (x, y from top-left of the content area)
APP_POS = (130, 160)
APPS_POS = (410, 160)

# Icon size
ICON_SIZE = 128


def build_bwsp(top, left, bottom, right, view_mode="icnv", show_sidebar=False):
    """Build the bwsp (Browser Window Settings Plist) blob."""
    import plistlib
    d = {
        "ShowSidebar": show_sidebar,
        "ShowStatusBar": False,
        "ShowToolbar": False,
        "ShowTabView": False,
        "ShowPathbar": False,
        "WindowBounds": "{{%d, %d}, {%d, %d}}" % (left, top, right - left, bottom - top),
        "SidebarWidth": 0,
        "PreviewPaneVisibility": False,
    }
    return plistlib.dumps(d, fmt=plistlib.FMT_BINARY)


def build_icvp(icon_size=128, text_size=14, arrange_by="none",
               bg_type=1, bg_r=0.12, bg_g=0.12, bg_b=0.16):
    """Build the icvp (Icon View Properties) plist blob.
    bg_type: 1 = color, 2 = image
    """
    import plistlib
    d = {
        "viewOptionsVersion": 1,
        "arrangeBy": arrange_by,
        "gridOffsetX": 0.0,
        "gridOffsetY": 0.0,
        "gridSpacing": 100.0,
        "iconSize": float(icon_size),
        "labelOnBottom": True,
        "showIconPreview": False,
        "showItemInfo": False,
        "textSize": float(text_size),
        "backgroundType": bg_type,
        "backgroundColorRed": bg_r,
        "backgroundColorGreen": bg_g,
        "backgroundColorBlue": bg_b,
    }
    return plistlib.dumps(d, fmt=plistlib.FMT_BINARY)


def main():
    with DSStore.open(OUTPUT_PATH, "w+") as d:
        # Root directory settings
        d["."]["bwsp"] = build_bwsp(
            top=WIN_Y, left=WIN_X,
            bottom=WIN_Y + WIN_HEIGHT, right=WIN_X + WIN_WIDTH
        )
        d["."]["icvp"] = build_icvp(
            icon_size=ICON_SIZE,
            text_size=14,
            bg_r=0.071, bg_g=0.071, bg_b=0.094  # Match launcher window color
        )

        # Icon positions
        d["ObeyCraftLauncher.app"]["Iloc"] = struct.pack(">II", *APP_POS)
        d["Applications"]["Iloc"] = struct.pack(">II", *APPS_POS)

    print(f"Generated: {OUTPUT_PATH}")
    print(f"  Window: {WIN_WIDTH}x{WIN_HEIGHT} at ({WIN_X},{WIN_Y})")
    print(f"  App icon: {APP_POS}")
    print(f"  Applications: {APPS_POS}")
    print(f"  Icon size: {ICON_SIZE}px")


if __name__ == "__main__":
    main()
