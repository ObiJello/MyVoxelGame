#!/usr/bin/env python3
"""Verify the archived reference modules and assets against the import hashes."""
from pathlib import Path
import hashlib
import json
import re
import sys

root = Path(__file__).resolve().parents[1]
manifest = json.loads((root / "docs/source_manifest.json").read_text())
errors = []
for entry in manifest["files"]:
    path = root / entry["destination"]
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
        errors.append(entry["destination"])
enum_source = (root / "original/reference-only/App_enums.h").read_text()
enum_port = (root / "platform/MinecraftColours.h").read_text()
pattern = r"enum eMinecraftColour\s*\{.*?\};"
if re.search(pattern, enum_source, re.S).group() != re.search(pattern, enum_port, re.S).group():
    errors.append("platform/MinecraftColours.h (original enum extraction)")
if errors:
    print("Imported files differ from the recorded source:\n" + "\n".join(errors), file=sys.stderr)
    sys.exit(1)
print(f"Verified {len(manifest['files'])} imported source/asset files.")
