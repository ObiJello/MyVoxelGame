"""Shared Tracy plumbing for the capture scripts (tracy_report.py,
analyze_trace.py, gpu_report.py).

    import tracy_tools
    ex = tracy_tools.export("capture.tracy")        # cached tracy-export run
    ex.info["frame_sets"], ex.thread_names, ex.rows("zone_stats")

The tools come from tools/tracy/, built by tools/build_tracy_tools.sh at the
Tracy version CMakeLists.txt pins; ensure_tools() rebuilds them when the pin
moves, so a Tracy upgrade never needs a manual tool rebuild. Exports are cached
per capture (size + mtime + tool version) under the tool cache dir, so a second
report on the same capture skips the export; the least recently used exports
are dropped past OBEY_TRACY_CACHE_GB (default 2).
"""
import csv
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TOOLS_DIR = REPO / "tools" / "tracy"
BUILD_SCRIPT = REPO / "tools" / "build_tracy_tools.sh"

csv.field_size_limit(1 << 30)


def cache_root():
    env = os.environ.get("OBEY_TRACY_CACHE")
    if env:
        return Path(env)
    if platform.system() == "Darwin":
        return Path.home() / "Library" / "Caches" / "obeycraft-tracy"
    return Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "obeycraft-tracy"


def pinned_tag():
    """The tracy GIT_TAG in CMakeLists.txt."""
    found = False
    for line in (REPO / "CMakeLists.txt").read_text().splitlines():
        if "github.com/wolfpld/tracy" in line:
            found = True
        elif found and line.strip().startswith("GIT_TAG"):
            return line.split()[1]
    sys.exit("tracy_tools: no tracy GIT_TAG in CMakeLists.txt")


_tools_checked = False


def ensure_tools():
    """Build tools/tracy/ at the pinned version if it is missing or stale."""
    global _tools_checked
    if _tools_checked:
        return
    check = subprocess.run([str(BUILD_SCRIPT), "--check"], capture_output=True, text=True)
    if check.returncode != 0:
        print(f"[tracy_tools] {check.stdout.strip()} — building the tools (one-off, ~1 min)", file=sys.stderr)
        built = subprocess.run([str(BUILD_SCRIPT)], stdout=sys.stderr, stderr=sys.stderr)
        if built.returncode != 0:
            sys.exit("tracy_tools: tools/build_tracy_tools.sh failed")
    _tools_checked = True


def tool(name):
    """Path to a tools/tracy/ binary (tracy-export, tracy-capture, tracy-csvexport, tracy-update)."""
    ensure_tools()
    return TOOLS_DIR / name


def export(capture, zones=False, out=None):
    """Export `capture` with tracy-export (cached) and return an Export.

    zones=True also writes the raw per-thread zone tables (zones/<tid>_*.csv) —
    large; the summary tables (zone_stats, frame_zones) never need them.
    """
    capture = Path(capture).resolve()
    if not capture.is_file():
        sys.exit(f"tracy_tools: no such capture: {capture}")
    exe = tool("tracy-export")
    st = capture.stat()
    stamp = {
        "capture": str(capture), "size": st.st_size, "mtime_ns": st.st_mtime_ns,
        "tools": (TOOLS_DIR / "VERSION").read_text().strip(), "exporter_mtime_ns": exe.stat().st_mtime_ns,
    }
    if out is None:
        key = hashlib.sha1(str(capture).encode()).hexdigest()[:10]
        out = cache_root() / "exports" / f"{capture.stem}-{key}"
    out = Path(out)
    stamp_file = out / ".stamp.json"
    if stamp_file.is_file():
        try:
            old = json.loads(stamp_file.read_text())
            if {k: v for k, v in old.items() if k != "zones"} == stamp and (old.get("zones") or not zones):
                os.utime(stamp_file)          # most recently used, for the pruning
                return Export(out)
        except (ValueError, OSError):
            pass

    shutil.rmtree(out, ignore_errors=True)
    print(f"[tracy_tools] exporting {capture.name}{' with raw zones' if zones else ''} ...", file=sys.stderr)
    cmd = [str(exe), str(capture), "-o", str(out)] + ([] if zones else ["--no-zones"])
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        msg = proc.stderr.strip()
        if proc.returncode == 2:
            msg += (f"\n  The pin is {pinned_tag()}. Set GIT_TAG in CMakeLists.txt to the viewer's version;"
                    " the tools rebuild themselves on the next run.")
        elif proc.returncode == 3:
            msg += f"\n  {TOOLS_DIR / 'tracy-update'} old.tracy new.tracy"
        sys.exit(f"tracy_tools: tracy-export failed ({proc.returncode}):\n  {msg}")
    stamp["zones"] = zones
    stamp_file.write_text(json.dumps(stamp))
    _prune_exports(keep=out)
    return Export(out)


def _dir_size(path):
    total = 0
    for root, _, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(root, f))
            except OSError:
                pass
    return total


def _prune_exports(keep):
    """Drop least-recently-used cached exports past OBEY_TRACY_CACHE_GB (default
    2 GB) — one export with raw zones is ~3 GB per minute of capture, and the
    dev Mac has little disk to spare."""
    root = cache_root() / "exports"
    limit = float(os.environ.get("OBEY_TRACY_CACHE_GB", "2")) * 2**30
    dirs = []
    for d in root.iterdir() if root.is_dir() else ():
        stamp = d / ".stamp.json"
        dirs.append((stamp.stat().st_mtime if stamp.is_file() else 0.0, d, _dir_size(d)))
    total = sum(size for _, _, size in dirs)
    for _, d, size in sorted(dirs, key=lambda x: x[0]):
        if total <= limit:
            break
        if d.resolve() == Path(keep).resolve():
            continue
        shutil.rmtree(d, ignore_errors=True)
        total -= size


class Export:
    """One tracy-export output folder."""

    def __init__(self, path):
        self.path = Path(path)
        self.info = json.loads((self.path / "info.json").read_text())
        self.threads = [dict(self._typed(r), name=r["name"]) for r in self.rows("threads")]
        self.thread_names = {t["tid"]: t["name"] for t in self.threads}

    @staticmethod
    def _typed(row):
        out = {}
        for k, v in row.items():
            if v == "":
                out[k] = None
                continue
            try:
                out[k] = int(v)
            except ValueError:
                try:
                    out[k] = float(v)
                except ValueError:
                    out[k] = v
        return out

    def rows(self, table, typed=False):
        """Stream a table's rows as dicts (strings, or typed=True for numbers)."""
        path = self.path / f"{table}.csv"
        if not path.is_file():
            return
        with open(path, newline="") as f:
            for row in csv.DictReader(f):
                yield self._typed(row) if typed else row

    def has_rows(self, table):
        path = self.path / f"{table}.csv"
        if not path.is_file():
            return False
        with open(path) as f:
            f.readline()
            return bool(f.readline())

    def thread_label(self, tid):
        return f"{self.thread_names.get(tid, '?')} ({tid})"

    def threads_named(self, name):
        return [t["tid"] for t in self.threads if t["name"] == name]

    def frames(self, frame_set):
        """[(start_ns, end_ns)] of a frame set, in order."""
        return [(int(r["start_ns"]), int(r["end_ns"])) for r in self.rows("frames") if r["frame_set"] == frame_set]

    def frame_set_names(self):
        return [fs["name"] for fs in self.info.get("frame_sets", [])]

    def plots(self, names=None):
        """{plot name: [(time_ns, value)]}, optionally only `names`."""
        want = set(names) if names else None
        out = {}
        for r in self.rows("plots"):
            if want is None or r["plot"] in want:
                out.setdefault(r["plot"], []).append((int(r["time_ns"]), float(r["value"])))
        return out

    def zone_files(self):
        """{tid: path} of the raw zone tables (needs export(zones=True))."""
        out = {}
        zdir = self.path / "zones"
        if zdir.is_dir():
            for p in zdir.glob("*.csv"):
                out[int(p.name.split("_", 1)[0])] = p
        return out

    def zones(self, tid):
        """Stream one thread's raw zones (typed)."""
        path = self.zone_files().get(tid)
        if not path:
            return
        with open(path, newline="") as f:
            rd = csv.reader(f)
            header = next(rd)
            ix = {c: i for i, c in enumerate(header)}
            i_id, i_par, i_dep, i_s, i_e, i_self, i_name = (ix[c] for c in (
                "id", "parent", "depth", "start_ns", "end_ns", "self_ns", "name"))
            i_text = ix["text"]
            for row in rd:
                yield (int(row[i_id]), int(row[i_par]), int(row[i_dep]), int(row[i_s]), int(row[i_e]),
                       int(row[i_self]), row[i_name], row[i_text])
