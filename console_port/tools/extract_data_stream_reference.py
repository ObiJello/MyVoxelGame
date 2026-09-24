#!/usr/bin/env python3
"""Build original primitive streams without the separate platform identity API."""
from pathlib import Path
import re
import sys
root = Path(__file__).resolve().parents[1]
out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
for name in ["DataInput", "DataOutput", "DataInputStream", "DataOutputStream"]:
    extensions = ["h"] if name in ["DataInput", "DataOutput"] else ["h", "cpp"]
    for extension in extensions:
        text = (root / f"original/Minecraft.World/{name}.{extension}").read_text()
        if extension == "h":
            text = "\n".join(line for line in text.splitlines() if not re.search(r"virtual .*PlayerUID", line)) + "\n"
        else:
            signature = "PlayerUID DataInputStream::readPlayerUID" if name == "DataInputStream" else "void DataOutputStream::writePlayerUID"
            start = text.index(signature); end = text.index("{", start) + 1; depth = 1
            while depth:
                depth += (text[end] == "{") - (text[end] == "}"); end += 1
            text = text[:start] + text[end:]
        text = re.sub(r"(?<![:\w])byte\b", "::byte", text)
        (out / f"{name}.{extension}").write_text(text)
