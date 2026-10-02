#!/usr/bin/env python3
"""Find class-only PlaceObject3 tags in the archived console menu movies.

These placements are valid SWF 9, but Ruffle 0.6.0 rejects them as an
"Invalid PlaceObject type". This is a static compatibility audit; it does not
modify the original files or display the game.
"""

from __future__ import annotations

import argparse
import json
import struct
import zlib
from collections import Counter
from pathlib import Path


SOURCE = Path(__file__).resolve().parents[1] / "source_full/Minecraft.Client"


def read_movie(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) < 12 or data[:3] not in (b"FWS", b"CWS"):
        raise ValueError(f"not an unencrypted SWF: {path}")
    if data[:3] == b"CWS":
        data = b"FWS" + data[3:8] + zlib.decompress(data[8:])
    declared_size = struct.unpack_from("<I", data, 4)[0]
    if len(data) != declared_size:
        raise ValueError(f"SWF size mismatch: {path}")
    return data


def first_tag(data: bytes) -> int:
    # RECT: 5 bits for Nbits and four signed Nbits coordinates.
    rect_bits = data[8] >> 3
    rect_bytes = (5 + 4 * rect_bits + 7) // 8
    return 8 + rect_bytes + 4  # frame rate + frame count


def audit(path: Path) -> dict:
    data = read_movie(path)
    counts: Counter[int] = Counter()
    class_only = []
    dependencies = []

    def walk(offset: int, end: int, owner: str) -> None:
        while offset + 2 <= end:
            header = struct.unpack_from("<H", data, offset)[0]
            offset += 2
            tag = header >> 6
            size = header & 63
            if size == 63:
                if offset + 4 > end:
                    raise ValueError(f"truncated extended tag length: {path}")
                size = struct.unpack_from("<I", data, offset)[0]
                offset += 4
            if offset + size > end:
                raise ValueError(f"tag {tag} overruns sprite {owner}: {path}")
            counts[tag] += 1
            if tag == 70 and size >= 4:
                flags = struct.unpack_from("<H", data, offset)[0]
                # MOVE/HAS_CHARACTER are low bits; HAS_CLASS_NAME is bit 11.
                if flags & 0x800 and not flags & 0x3:
                    class_only.append({
                        "sprite": owner,
                        "depth": struct.unpack_from("<H", data, offset + 2)[0],
                    })
            elif tag == 71:
                terminator = data.find(b"\0", offset, offset + size)
                if terminator != -1:
                    dependencies.append(data[offset:terminator].decode("utf-8", "replace"))
            elif tag == 39 and size >= 4:
                sprite_id = struct.unpack_from("<H", data, offset)[0]
                walk(offset + 4, offset + size, str(sprite_id))
            offset += size
            if tag == 0:
                break

    walk(first_tag(data), len(data), "root")
    return {
        "file": str(path.relative_to(SOURCE)),
        "place_object_3": counts[70],
        "class_only_placements": len(class_only),
        "class_only_examples": class_only[:8],
        "imported_libraries": dependencies,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("movies", nargs="*", type=Path)
    args = parser.parse_args()
    movies = args.movies or [
        SOURCE / "Common/Media/MainMenu720.swf",
        SOURCE / "Common/Media/Panorama720.swf",
        SOURCE / "Common/Media/skin.swf",
        SOURCE / "PS3Media/Media/skinPS3.swf",
    ]
    print(json.dumps([audit(path.resolve()) for path in movies], indent=2))


if __name__ == "__main__":
    main()
