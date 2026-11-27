"""Build the macOS Yoga/Core Text backend. Called by python3 build.py ui."""
from pathlib import Path
import hashlib
import subprocess
import tarfile

HERE = Path(__file__).resolve().parent
BUILD = HERE.parents[1] / "build"
CACHE = BUILD / "ui"
VERSION = "3.2.1"
SHA = "86b399ac31fd820d8ffa823c3fae31bb690b6fc45301b2a8a966c09b5a088b55"

def build():
    CACHE.mkdir(parents=True, exist_ok=True)
    src = CACHE / ("yoga-" + VERSION)
    archive = CACHE / (src.name + ".tar.gz")
    if not (src / "yoga/Yoga.h").exists():
        subprocess.run(["curl", "-fL", "--retry", "2", "-o", archive,
                        "https://codeload.github.com/facebook/yoga/tar.gz/refs/tags/v" + VERSION], check=True)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA:
            raise ValueError("Yoga release checksum mismatch")
        with tarfile.open(archive) as tar:
            tar.extractall(CACHE, filter="data")
    obj = CACHE / "obj"
    subprocess.run(["cmake", "-S", src/"yoga", "-B", obj, "-DCMAKE_BUILD_TYPE=Release"], check=True)
    subprocess.run(["cmake", "--build", obj, "--parallel", "6"], check=True)
    lib = BUILD/"libfutsurface.dylib"
    deps = [HERE/"surface.mm", obj/"libyogacore.a"]
    if not lib.exists() or max(p.stat().st_mtime for p in [*deps, HERE/"glyphs.h"]) > lib.stat().st_mtime:
        subprocess.run(["clang++", "-std=c++20", "-O2", "-fobjc-arc", "-dynamiclib", "-I", src,
                        *deps, "-framework", "AppKit", "-framework", "CoreText", "-framework", "CoreGraphics",
                        "-Wl,-exported_symbol,_us_*", "-o", lib], check=True)

if __name__ == "__main__":
    build()
