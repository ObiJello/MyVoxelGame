#!/bin/bash
# promote_goldens.sh - promote Java reference dumps in results/ to goldens/.
#
# Every results/java_<tag>.txt is a valid Java reference (the Java side never
# changes), so all well-formed dumps are promoted, gzipped, and recorded in
# goldens/MANIFEST.json with checksums and provenance. Dumps whose stored
# summary_<tag>.json shows nonzero mismatches are still promoted (those were
# stale C++-side failures) but flagged "summary_stale": true so a later
# regression failure there is interpreted correctly.
#
# Never overwrites an existing golden with different content (exit 2).
#
# Usage: ./promote_goldens.sh [--manifest-only]
#   --manifest-only   Rebuild MANIFEST.json from goldens/ without promoting
#                     anything new from results/.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec python3 - "$SCRIPT_DIR" "$@" <<'PYEOF'
import gzip as gzmod
import hashlib, json, os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone

script_dir = sys.argv[1]
manifest_only = "--manifest-only" in sys.argv[2:]
results = os.path.join(script_dir, "results")
goldens = os.path.join(script_dir, "goldens")
os.makedirs(goldens, exist_ok=True)
manifest_path = os.path.join(goldens, "MANIFEST.json")

TAG_RE = re.compile(
    r"^java_s(?P<seed>-?\d+)_(?:single_(?P<sx>-?\d+)_(?P<sz>-?\d+)"
    r"|r(?P<r>\d+)_c(?P<cx>-?\d+)_(?P<cz>-?\d+))_p(?P<phases>[\d-]+)"
    r"(?P<be>_be)?(?:_wt(?P<wt>[a-z0-9_]+(?:-[a-z0-9_]+)*))?"
    r"(?:_d(?P<dim>nether|end))?\.txt(?P<gz>\.gz)?$"
)

def sha256_file(path, chunk=1 << 22):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(chunk), b""):
            h.update(blk)
    return h.hexdigest()

def sha256_gunzipped(path, chunk=1 << 22):
    h = hashlib.sha256()
    n = 0
    with gzmod.open(path, "rb") as f:
        for blk in iter(lambda: f.read(chunk), b""):
            h.update(blk)
            n += len(blk)
    return h.hexdigest(), n

def parse_tag(fname):
    m = TAG_RE.match(fname)
    if not m:
        return None
    d = m.groupdict()
    tag = fname[len("java_"):]
    tag = tag[:-len(".txt.gz")] if fname.endswith(".gz") else tag[:-len(".txt")]
    if d["sx"] is not None:
        mode, radius, center = "single", 0, [int(d["sx"]), int(d["sz"])]
        tier = 0
    else:
        mode, radius, center = "radius", int(d["r"]), [int(d["cx"]), int(d["cz"])]
        tier = 2 if radius >= 10 else 1
    # World-type segment: "_wt<type>[-<preset|biome>]". Custom flat layer
    # strings are tagged with an irreversible hash ("-x<8hex>") and cannot be
    # reconstructed into a regression command - refuse to promote those.
    world_type, flat_preset, single_biome = "default", "", ""
    if d.get("wt"):
        parts = d["wt"].split("-")
        world_type = parts[0]
        for extra in parts[1:]:
            if re.fullmatch(r"x[0-9a-f]{8}", extra):
                print(f"  SKIP (custom flat layers not re-gateable from tag): {fname}")
                return None
            if world_type == "flat":
                flat_preset = extra
            elif world_type == "single_biome_surface":
                single_biome = "minecraft:" + extra
    return dict(tag=tag, seed=int(d["seed"]), mode=mode, radius=radius,
                center=center, phases=d["phases"], tier=tier,
                be=d.get("be") is not None,
                dimension=d.get("dim") or "overworld",
                world_type=world_type, flat_preset=flat_preset,
                single_biome=single_biome)

def summary_state(tag):
    """(exists, total_mismatches or None)"""
    p = os.path.join(results, f"summary_{tag}.json")
    if not os.path.isfile(p):
        return False, None
    try:
        with open(p) as f:
            s = json.load(f)
        return True, s.get("total_mismatches", s.get("total"))
    except Exception:
        return True, None

def looks_complete(path):
    """Cheap structural sanity: non-empty, last line is a complete record."""
    size = os.path.getsize(path)
    if size < 1000:
        return False
    with open(path, "rb") as f:
        f.seek(max(0, size - 4096))
        tail = f.read().decode("utf-8", "replace")
    if not tail.endswith("\n"):
        return False
    last = tail.rstrip("\n").rsplit("\n", 1)[-1]
    return bool(re.match(r"^[BQHCSPRE],", last)) or last.startswith("#")

entries = {}

# ---- load existing manifest (preserve created dates/provenance) ----
old = {}
if os.path.isfile(manifest_path):
    try:
        with open(manifest_path) as f:
            for e in json.load(f).get("goldens", []):
                old[e["tag"]] = e
    except Exception:
        pass

def process_existing_golden(fname):
    meta = parse_tag(fname)
    if not meta:
        print(f"  SKIP (unparseable): {fname}")
        return None
    gpath = os.path.join(goldens, fname)
    prev = old.get(meta["tag"])
    if prev and prev.get("sha256_gz") and os.path.getsize(gpath) == prev.get("bytes_gz"):
        # trust cached hashes when size matches; integrity is re-checked by run_regression
        meta.update({k: prev[k] for k in
                     ("sha256_plain", "bytes_plain", "sha256_gz", "bytes_gz",
                      "created", "source", "summary_stale", "purpose") if k in prev})
        return meta
    plain_sha, plain_bytes = sha256_gunzipped(gpath)
    meta.update(sha256_plain=plain_sha, bytes_plain=plain_bytes,
                sha256_gz=sha256_file(gpath), bytes_gz=os.path.getsize(gpath),
                created=(prev or {}).get("created") or
                        datetime.fromtimestamp(os.path.getmtime(gpath), timezone.utc).isoformat(timespec="seconds"),
                source=(prev or {}).get("source", "pre-existing"))
    return meta

def promote(fname):
    meta = parse_tag(fname)
    if not meta:
        print(f"  SKIP (unparseable): {fname}")
        return None
    src = os.path.join(results, fname)
    dst = os.path.join(goldens, fname + ".gz")
    if not looks_complete(src):
        print(f"  SKIP (dump looks truncated/incomplete): {fname}")
        return None
    plain_sha = sha256_file(src)
    if os.path.isfile(dst):
        g_sha, _ = sha256_gunzipped(dst)
        if g_sha != plain_sha:
            print(f"ERROR: golden {dst} exists with DIFFERENT content than {src}; refusing.")
            sys.exit(2)
        print(f"  identical golden already present: {meta['tag']}")
        return None  # will be picked up by the existing-golden pass
    subprocess.run(["gzip", "-c", src], stdout=open(dst, "wb"), check=True)
    have_summary, total = summary_state(meta["tag"])
    meta.update(sha256_plain=plain_sha, bytes_plain=os.path.getsize(src),
                sha256_gz=sha256_file(dst), bytes_gz=os.path.getsize(dst),
                created=datetime.now(timezone.utc).isoformat(timespec="seconds"),
                source="promoted-from-results",
                summary_stale=(not have_summary or (total or 0) != 0))
    stale = " [summary_stale]" if meta["summary_stale"] else ""
    print(f"  promoted: {meta['tag']} ({meta['bytes_plain']/1e6:.1f} MB -> {meta['bytes_gz']/1e6:.1f} MB){stale}")
    return meta

if not manifest_only:
    cands = sorted(f for f in os.listdir(results) if TAG_RE.match(f) and f.endswith(".txt"))
    print(f"Promoting {len(cands)} Java dumps from results/ ...")
    with ThreadPoolExecutor(max_workers=4) as ex:
        for meta in ex.map(promote, cands):
            if meta:
                entries[meta["tag"]] = meta

# ---- always sweep goldens/ so manifest reflects reality ----
gfiles = sorted(f for f in os.listdir(goldens) if TAG_RE.match(f) and f.endswith(".gz"))
todo = [f for f in gfiles if parse_tag(f) and parse_tag(f)["tag"] not in entries]
if todo:
    print(f"Indexing {len(todo)} existing goldens ...")
    with ThreadPoolExecutor(max_workers=4) as ex:
        for meta in ex.map(process_existing_golden, todo):
            if meta:
                entries[meta["tag"]] = meta

# ---- environment provenance ----
jar = None
cp = os.path.join(script_dir, "minecraft_classpath.txt")
if os.path.isfile(cp):
    for part in open(cp).read().split(":"):
        if part.strip().endswith("26.1-snapshot-1.jar"):
            jar = part.strip()
            break

manifest = {
    "jar": {"version": "26.1-snapshot-1", "path": jar,
            "sha256": sha256_file(jar) if jar and os.path.isfile(jar) else None},
    "format_sha256": sha256_file(os.path.join(script_dir, "FORMAT.md")),
    "comparator_sha256": sha256_file(os.path.join(script_dir, "compare_parity.py")),
    "updated": datetime.now(timezone.utc).isoformat(timespec="seconds"),
    "goldens": sorted(entries.values(), key=lambda e: (e["tier"], e["tag"])),
}
with open(manifest_path, "w") as f:
    json.dump(manifest, f, indent=1)
    f.write("\n")

by_tier = {}
for e in entries.values():
    by_tier[e["tier"]] = by_tier.get(e["tier"], 0) + 1
print(f"\nMANIFEST.json written: {len(entries)} goldens "
      f"(tier0={by_tier.get(0,0)} tier1={by_tier.get(1,0)} tier2={by_tier.get(2,0)})")
print("NOTE: raw java_*.txt dumps in results/ are now redundant and may be deleted by hand.")
PYEOF
