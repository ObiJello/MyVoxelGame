#!/usr/bin/env python3
"""Build and exercise the archived Iggy runtime without showing a game window."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import sys
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / "tools/iggy_reference"
SOURCE = ROOT / "source_full/Minecraft.Client"
IGGY = SOURCE / "Windows64/Iggy"
WINE_URL = "https://github.com/Gcenx/macOS_Wine_builds/releases/download/11.0_1/wine-stable-11.0_1-osx64.tar.xz"
WINE_SHA256 = "b50dc50ec7f41d58b115a6b685d4d1315ba3c797bd3aa0f49213f2703cb82388"


def setup_wine(build: Path) -> Path:
    wine = build / "Wine Stable.app/Contents/Resources/wine/bin/wine"
    if wine.exists():
        return wine
    archive = build / "wine.tar.xz"
    build.mkdir(parents=True, exist_ok=True)
    print("Downloading the pinned Wine runtime...", file=sys.stderr, flush=True)
    with urllib.request.urlopen(WINE_URL, timeout=60) as response, archive.open("wb") as out:
        shutil.copyfileobj(response, out)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != WINE_SHA256:
        archive.unlink()
        raise RuntimeError("Wine archive checksum mismatch")
    subprocess.run(["tar", "-xf", str(archive), "-C", str(build)], check=True)
    return wine


def build_host(build: Path) -> Path:
    cc = shutil.which("x86_64-w64-mingw32-gcc")
    cxx = shutil.which("x86_64-w64-mingw32-g++")
    if not cc or not cxx:
        raise RuntimeError("Install the cross compiler with: brew install mingw-w64")
    build.mkdir(parents=True, exist_ok=True)
    json_roots = sorted(ROOT.parent.glob("cmake-build-*/_deps/nlohmann_json-src/single_include"))
    if not json_roots:
        raise RuntimeError("The project's nlohmann/json library is missing")
    sysroot = Path(subprocess.check_output([cc, "-print-sysroot"], text=True).strip())
    include = ["-I", str(IGGY / "include"), "-I", str(HERE),
               "-I", str(ROOT.parent / "ext/stb_image")]
    jobs = [
        ([cc, "-O2", "-DNDEBUG", "-include", str(HERE / "GDrawCompilerCompat.h"),
          *include, "-I", str(sysroot / "x86_64-w64-mingw32/include/GL"),
          "-c", str(IGGY / "gdraw/gdraw_wgl.c"), "-o", str(build / "gdraw.o")]),
    ]
    for name in ("UIFontData", "UIBitmapFont"):
        jobs.append([cxx, "-O2", "-D_CONTENT_PACKAGE", *include,
                     "-c", str(SOURCE / f"Common/UI/{name}.cpp"),
                     "-o", str(build / f"{name}.o")])
    for command in jobs:
        subprocess.run(command, check=True, cwd=ROOT)
    executable = build / "iggy_reference.exe"
    subprocess.run([
        cxx, "-O2", "-std=c++17", "-static", *include,
        "-I", str(IGGY / "gdraw"), "-I", str(SOURCE / "Common/UI"),
        "-I", str(json_roots[0]), str(HERE / "ReferenceHost.cpp"),
        str(build / "gdraw.o"), str(build / "UIFontData.o"), str(build / "UIBitmapFont.o"),
        str(IGGY / "lib/iggy_w64.lib"), "-lopengl32", "-lgdi32", "-luser32",
        "-o", str(executable),
    ], check=True, cwd=ROOT)
    shutil.copy2(IGGY / "lib/redist64/iggy_w64.dll", build)
    return executable


class Reference:
    def __init__(self, wine: Path, build: Path):
        env = os.environ.copy()
        env.update(WINEPREFIX=str(build / "prefix"), WINEDEBUG="-all",
                   MVK_CONFIG_LOG_LEVEL="0",
                   WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d;winedbg.exe=d")
        self.process = subprocess.Popen(
            [str(wine), str(build / "iggy_reference.exe")], cwd=ROOT, env=env,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
        self.pending = bytearray()
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        try:
            self.ready = json.loads(self.read_line(90))
            if not self.ready.get("ready"):
                raise RuntimeError(str(self.ready))
        except BaseException:
            self.close()
            raise

    def receive(self, deadline):
        if not self.selector.select(max(0, deadline-time.monotonic())):
            raise TimeoutError("Original Iggy host did not respond")
        chunk = os.read(self.process.stdout.fileno(), 1024*1024)
        if not chunk:
            raise RuntimeError("Original Iggy host exited: " + self.process.stderr.read().decode(errors="replace"))
        self.pending.extend(chunk)

    def read_line(self, timeout=30):
        deadline = time.monotonic()+timeout
        while b"\n" not in self.pending:
            self.receive(deadline)
        end = self.pending.index(b"\n")
        line = bytes(self.pending[:end])
        del self.pending[:end+1]
        return line

    def request(self, **request):
        self.process.stdin.write(json.dumps(request, ensure_ascii=True).encode()+b"\n")
        response = json.loads(self.read_line())
        if not response.get("ok"):
            raise RuntimeError(str(response))
        count = response.get("bytes", 0)
        deadline = time.monotonic()+30
        while len(self.pending)<count:
            self.receive(deadline)
        pixels = bytes(self.pending[:count])
        del self.pending[:count]
        return response, pixels

    def close(self):
        if self.process.poll() is None:
            try:
                self.request(op="quit")
                self.process.wait(timeout=10)
            except Exception:
                self.process.kill()
                self.process.wait(timeout=10)
        self.selector.close()
        for stream in (self.process.stdin,self.process.stdout,self.process.stderr):
            stream.close()


def verify(reference: Reference) -> dict:
    report = {"runtime": reference.ready, "checks": [], "callbacks": []}

    def request(**kwargs):
        response, pixels = reference.request(**kwargs)
        if response.get("warnings"):
            raise RuntimeError(str(response["warnings"]))
        report["callbacks"].extend(response.get("callbacks", []))
        return response, pixels

    def call(path, method, args):
        return request(op="call", slot="menu", path=path, method=method, args=args)

    request(op="load", slot="menu", movie="MainMenu720.swf")
    # UIScene_MainMenu.cpp: initialize all six controls, then apply PS3 removals.
    labels = ("Play Game", "Leaderboards", "Achievements", "Help & Options", "Minecraft Store", "Exit Game")
    for i, label in enumerate(labels):
        call(f"Button{i+1}", "Init", [label, i])
    call("", "RemoveObject", ["Button6", False])
    call("", "RemoveObject", ["Button3", False])
    call("", "SetFocus", [-1])
    for prop, expected in (("x",415),("y",250)):
        response,_ = request(op="get",slot="menu",path="Button1",property=prop)
        if response["value"] != expected:
            raise AssertionError((prop,response))
    report["checks"].append("Original PS3 main menu constructs and initializes its controls")
    a, first = request(op="render",slots=["menu"],pixels=True)
    b, repeated = request(op="render",slots=["menu"],pixels=True)
    assert first == repeated and a["nontransparent"] > 20000
    report["checks"].append("Rendering a paused movie twice produces identical RGBA bytes")
    report["initial_frame"] = {"sha256":hashlib.sha256(first).hexdigest(),
                               "nontransparent":a["nontransparent"],"custom_draws":a["custom_draws"]}
    request(op="key",slot="menu",code=40,down=True)
    request(op="key",slot="menu",code=40,down=False)
    _, focused = request(op="render",slots=["menu"],pixels=True)
    assert first != focused
    assert any(c["name"] == "handleFocusChange" for c in report["callbacks"])
    report["checks"].append("Original ActionScript changes focus and the rendered button state")
    request(op="key",slot="menu",code=13,down=True)
    request(op="key",slot="menu",code=13,down=False)
    timeline = []
    for frame in range(31):
        response,_ = request(op="tick",slot="menu",seconds=frame/30)
        rendered, pixels = request(op="render",slots=["menu"],pixels=True)
        timeline.append({"frame":frame,"sha256":hashlib.sha256(pixels).hexdigest(),
                         "callbacks":response["callbacks"]})
    assert any(c["name"] == "handlePress" for c in report["callbacks"])
    report["checks"].append("Original button press timeline reaches its game callback")
    report["button_press"] = timeline
    # Test the other main-menu layers independently of the C++ reconstruction.
    request(op="load",slot="panorama",movie="Panorama720.swf")
    request(op="load",slot="logo",movie="ComponentLogo720.swf")
    response, _ = request(op="render",slots=["panorama","logo","menu"],pixels=True)
    assert response["nontransparent"] == 1280*720
    report["checks"].append("Original panorama, PS3 logo and menu layers compose to a full frame")
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["build","setup-wine","verify"])
    parser.add_argument("--build-dir",type=Path,default=ROOT/"build/iggy_reference")
    parser.add_argument("--wine",type=Path)
    args=parser.parse_args()
    build=args.build_dir.resolve()
    if args.action=="setup-wine":
        print(setup_wine(build));return
    build_host(build)
    if args.action=="build":
        print(build/"iggy_reference.exe");return
    wine=args.wine or build/"Wine Stable.app/Contents/Resources/wine/bin/wine"
    if not wine.exists():
        raise RuntimeError("Wine is missing; run the setup-wine action or pass --wine")
    reference=Reference(wine.resolve(),build)
    try:
        report=verify(reference)
    finally:
        reference.close()
    target=build/"verification.json"
    target.write_text(json.dumps(report,indent=2)+"\n")
    print(json.dumps({"passed":len(report["checks"]),"checks":report["checks"],"report":str(target)},indent=2))


if __name__=="__main__":
    main()
