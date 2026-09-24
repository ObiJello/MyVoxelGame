#!/usr/bin/env python3
"""Compile memory-stream originals with only C++20 byte-cast qualification."""
from pathlib import Path
import sys
root = Path(__file__).resolve().parents[1]
destination = Path(sys.argv[1])
destination.mkdir(parents=True, exist_ok=True)
for module in ["ByteArrayInputStream", "ByteArrayOutputStream", "BufferedOutputStream"]:
    source = (root / f"original/Minecraft.World/{module}.cpp").read_text()
    (destination / f"{module}.cpp").write_text(source.replace("(byte)", "(::byte)"))
