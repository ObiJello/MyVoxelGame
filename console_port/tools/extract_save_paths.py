#!/usr/bin/env python3
"""Extract PS3 dimension prefixes, initial file order and level format identifier."""
from pathlib import Path
import re
r=Path(__file__).resolve().parents[1];original=r/'original/reference-only'
level=(original/'LevelStorage.cpp').read_text();chunk=(original/'McRegionChunkStorage.cpp').read_text();region=(original/'McRegionLevelStorage.h').read_text()
nether=re.search(r'NETHER_FOLDER = (L"[^"]*")',level)[1];end=re.search(r'ENDER_FOLDER = (L"[^"]*")',level)[1]
names=re.findall(r'createFile\(ConsoleSavePath\((L"[^"]*")\)\)',chunk)
assert len(names)==12
version=re.search(r'MCREGION_VERSION_ID = (0x[0-9a-f]+)',region)[1]
out='#pragma once\n// Extracted from LevelStorage, McRegionChunkStorage and McRegionLevelStorage.\n#include <array>\nnamespace console::savepath {\n'
out+=f'inline constexpr auto nether={nether},end={end};\ninline constexpr int regionVersion={version};\n'
out+='inline constexpr std::array<const wchar_t*,12> initialRegions{'+','.join(names)+'};\n}\n'
(r/'platform/save/ConsoleSavePaths.h').write_text(out)
