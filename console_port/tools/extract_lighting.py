#!/usr/bin/env python3
"""Preserve the original Level lighting algorithm in its uncached CPU mode."""
from pathlib import Path
import sys
root = Path(__file__).resolve().parents[1]
source = (root / "original/reference-only/Level.cpp").read_text()
signatures = ["void Level::checkLight(int x", "int Level::getExpectedSkyColor(",
              "int Level::getExpectedBlockColor(", "void Level::checkLight(LightLayer::variety"]
parts = ['// Extracted original CPU lighting; cache disabled and lock made exception-safe.\n#include "WorldGenLevel.h"\n']
for signature in signatures:
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    body = source[start:end]
    body = body.replace("(lightCache_t *)TlsGetValue(tlsIdxLightCache)", "nullptr")
    body = body.replace("EnterCriticalSection(&m_checkLightCS);", "std::lock_guard<std::mutex> guard(m_checkLightCS);")
    body = body.replace("LeaveCriticalSection(&m_checkLightCS);", "")
    parts.append(body + "\n")
Path(sys.argv[1]).write_text("\n".join(parts))
