#!/usr/bin/env python3
"""Copy Minecraft's sound assets out of a local Minecraft install into assets/.

The game's sound engine (src/client/sound/) reads MC's own files:

    assets/sounds.json          <- minecraft/sounds.json
    assets/sounds/**/*.ogg      <- minecraft/sounds/**/*.ogg

They are NOT in the repository — a few hundred MB of Mojang's assets — and
.gitignore keeps them out. Every developer machine (and every release build)
runs this once. Without them the game starts silent and says so in the log.

Where the files come from: the launcher's hashed object store,

    <minecraft dir>/assets/indexes/<index>.json      the path -> hash map
    <minecraft dir>/assets/objects/<hh>/<hash>       the files themselves

The newest index is used unless --index names one (26.x's is 34.json).

Idempotent: a file whose size and SHA-1 already match its index entry is left
alone, and files under assets/sounds/ that the index no longer lists are
removed, so re-running after a Minecraft update leaves exactly that version's
sounds.

    python3 tools/extract_mc_sounds.py            # copy what is missing / stale
    python3 tools/extract_mc_sounds.py --check    # report only; exit 1 if out of date
    python3 tools/extract_mc_sounds.py --minecraft-dir PATH --index 34
"""

import argparse
import hashlib
import json
import os
import platform
import shutil
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(REPO, "assets")
OUT_JSON = os.path.join(ASSETS, "sounds.json")
OUT_DIR = os.path.join(ASSETS, "sounds")
# What was extracted, from which index: the build's staging step
# (cmake/StageSounds.cmake) re-copies the tree into the app only when this
# changes, instead of comparing hundreds of MB on every build.
STAMP = os.path.join(OUT_DIR, ".extracted")
PREFIX = "minecraft/sounds/"


def default_minecraft_dir():
    system = platform.system()
    home = os.path.expanduser("~")
    if system == "Darwin":
        return os.path.join(home, "Library", "Application Support", "minecraft")
    if system == "Windows":
        return os.path.join(os.environ.get("APPDATA", home), ".minecraft")
    return os.path.join(home, ".minecraft")


def pick_index(indexes_dir, wanted):
    if wanted:
        path = os.path.join(indexes_dir, wanted if wanted.endswith(".json") else wanted + ".json")
        if not os.path.isfile(path):
            raise SystemExit("index not found: " + path)
        return path
    candidates = []
    for name in os.listdir(indexes_dir):
        if not name.endswith(".json"):
            continue
        stem = name[:-5]
        # The launcher's numbered indexes (16, 17, ... 34) are the modern
        # ones; "1.8" / "1.12" are legacy. Highest number wins.
        try:
            candidates.append((int(stem), name))
        except ValueError:
            continue
    if not candidates:
        raise SystemExit("no numbered asset index in " + indexes_dir)
    return os.path.join(indexes_dir, max(candidates)[1])


def sha1_of(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def up_to_date(dest, entry):
    try:
        if os.path.getsize(dest) != entry["size"]:
            return False
    except OSError:
        return False
    return sha1_of(dest) == entry["hash"]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--minecraft-dir", default=default_minecraft_dir(),
                    help="the Minecraft install (default: the platform's launcher directory)")
    ap.add_argument("--index", default=None, help="asset index name, e.g. 34 (default: newest)")
    ap.add_argument("--check", action="store_true",
                    help="report what is missing or stale without copying; exit 1 if anything is")
    args = ap.parse_args()

    assets_dir = os.path.join(args.minecraft_dir, "assets")
    index_path = pick_index(os.path.join(assets_dir, "indexes"), args.index)
    with open(index_path) as f:
        objects = json.load(f)["objects"]

    wanted = {}
    for key, entry in objects.items():
        if key == "minecraft/sounds.json":
            wanted[OUT_JSON] = entry
        elif key.startswith(PREFIX) and key.endswith(".ogg"):
            wanted[os.path.join(OUT_DIR, *key[len(PREFIX):].split("/"))] = entry
    if OUT_JSON not in wanted:
        raise SystemExit("%s has no minecraft/sounds.json" % index_path)

    total_bytes = sum(e["size"] for e in wanted.values())
    digest = hashlib.sha1()
    for dest in sorted(wanted):
        digest.update(("%s %s %d\n" % (os.path.relpath(dest, ASSETS), wanted[dest]["hash"],
                                        wanted[dest]["size"])).encode())
    stamp_text = "%s %s\n" % (os.path.basename(index_path), digest.hexdigest())
    print("index %s: %d files, %.1f MB" % (os.path.basename(index_path), len(wanted), total_bytes / 1e6))

    stale, missing_objects = [], []
    for dest, entry in sorted(wanted.items()):
        if up_to_date(dest, entry):
            continue
        src = os.path.join(assets_dir, "objects", entry["hash"][:2], entry["hash"])
        if not os.path.isfile(src):
            missing_objects.append(dest)
            continue
        stale.append((src, dest))

    # Files a previous extraction left behind that this index does not list.
    extra = []
    if os.path.isdir(OUT_DIR):
        for root, _dirs, files in os.walk(OUT_DIR):
            for name in files:
                path = os.path.join(root, name)
                if path not in wanted and path != STAMP:
                    extra.append(path)

    try:
        with open(STAMP) as f:
            stamp_current = f.read() == stamp_text
    except OSError:
        stamp_current = False

    if args.check:
        print("  %d to copy, %d to remove, %d missing from the object store%s"
              % (len(stale), len(extra), len(missing_objects), "" if stamp_current else ", stamp stale"))
        for p in missing_objects[:10]:
            print("  ! not downloaded: " + os.path.relpath(p, REPO))
        sys.exit(1 if (stale or extra or missing_objects or not stamp_current) else 0)

    copied = 0
    for src, dest in stale:
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        tmp = dest + ".tmp"
        shutil.copyfile(src, tmp)
        os.replace(tmp, dest)
        copied += 1
    for path in extra:
        os.remove(path)
    # Directories emptied by the removals.
    if os.path.isdir(OUT_DIR):
        for root, dirs, files in os.walk(OUT_DIR, topdown=False):
            if root != OUT_DIR and not dirs and not files:
                os.rmdir(root)

    print("  copied %d, removed %d, %d already current" % (copied, len(extra), len(wanted) - copied - len(missing_objects)))
    if not missing_objects and (copied or extra or not stamp_current):
        with open(STAMP, "w") as f:
            f.write(stamp_text)
    if missing_objects:
        print("  ! %d files are not in the object store (launch that Minecraft version once to download them):"
              % len(missing_objects))
        for p in missing_objects[:10]:
            print("    " + os.path.relpath(p, REPO))
        sys.exit(1)


if __name__ == "__main__":
    main()
