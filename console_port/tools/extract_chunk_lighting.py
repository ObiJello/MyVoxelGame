#!/usr/bin/env python3
"""Extract LevelChunk light methods onto flat-storage views."""
from pathlib import Path
import sys
root = Path(__file__).resolve().parents[1]
source = (root / "original/reference-only/LevelChunk.cpp").read_text()
signatures = ["void LevelChunk::recalcHeightmap()", "void LevelChunk::lightLava()",
              "void LevelChunk::lightGaps(", "void LevelChunk::recheckGaps(",
              "void LevelChunk::lightGap(int x, int z, int source)",
              "void LevelChunk::lightGap(int x, int z, int y1, int y2)",
              "void LevelChunk::recalcHeight("]
out = ['// Original chunk lighting with flat-storage views.\n#include "GenerationChunkLight.h"\n']
for signature in signatures:
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    body = source[start:end].replace("LevelChunk::", "GenerationChunkLight::").replace("(byte)", "(::byte)")
    # The host retains height 256 and initializes emission in both sections.
    # The reference keeps the original lower-section-only initialization.
    # Brightening/darkening propagation retains the original operations/order.
    if "--host-height" in sys.argv:
        body = body.replace("int min = Level::maxBuildHeight - 1;", "int min = Level::maxBuildHeight;")
        body = body.replace("int y = Level::maxBuildHeight - 1;", "int y = Level::maxBuildHeight;")
        body = body.replace("(::byte) y", "y").replace("heightmap[z << 4 | x] & 0xff", "heightmap[z << 4 | x]")
        body = body.replace("heightmap[_z << 4 | _x] & 0xff", "heightmap[_z << 4 | _x]")
        if signature == "void LevelChunk::lightLava()":
            body = body.replace("y < Level::COMPRESSED_CHUNK_SECTION_HEIGHT", "y < Level::maxBuildHeight")
            body = body.replace("CompressedTileStorage *blocks = lowerBlocks;",
                                "CompressedTileStorage *blocks = y < Level::COMPRESSED_CHUNK_SECTION_HEIGHT ? lowerBlocks : upperBlocks;")
            body = body.replace("blocks->get(x,y,z)", "blocks->get(x,y % Level::COMPRESSED_CHUNK_SECTION_HEIGHT,z)")
    out.append(body + "\n")
Path(sys.argv[1]).write_text("\n".join(out))
