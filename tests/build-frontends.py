#!/usr/bin/env python3
"""Build and inspect all ARM64 frontends with an explicit PIC Dobby archive."""
import argparse
from pathlib import Path
import platform
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def run(args, *, succeeds=True):
    result = subprocess.run(args, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    print(result.stdout, end="", flush=True)
    if succeeds:
        result.check_returncode()
    elif result.returncode == 0 or "ARM64 builds require" not in result.stdout:
        raise RuntimeError("Expected an actionable ARM64 dependency error")


def inspect(binary):
    symbols = subprocess.check_output(["nm", "--defined-only", binary], text=True)
    dynamic = subprocess.check_output(["nm", "-D", binary], text=True)
    for name in ["DobbyHook", "DobbyDestroy"]:
        if not re.search(r"\bt " + name + r"$", symbols, re.MULTILINE):
            raise RuntimeError(f"{binary}: missing local {name}")
    if re.search(r"\bDobby\w*", dynamic):
        raise RuntimeError(f"{binary}: Dobby symbols escaped into the dynamic namespace")
    if "UNIQUE" in subprocess.check_output(["readelf", "-Ws", binary], text=True):
        raise RuntimeError(f"{binary}: GNU unique symbols prevent normal plugin unload")
    print(f"PASS {binary}: embedded hooks, hidden Dobby symbols, no GNU unique symbols", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dobby_include", type=Path)
    parser.add_argument("dobby_library", type=Path)
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if platform.machine() not in ["aarch64", "arm64"]:
        parser.error("Run this check on Linux ARM64")
    include, library = args.dobby_include.resolve(), args.dobby_library.resolve()
    if not (include / "dobby.h").is_file() or not library.is_file() or library.suffix != ".a":
        parser.error("Supply the Dobby include directory and PIC libdobby.a archive")
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    (ROOT / ".build").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=ROOT / ".build", prefix="frontend-errors-") as temporary:
        for label, header, archive in [("missing", "", ""),
                                      ("header", str(include / "missing"), str(library)),
                                      ("archive", str(include), str(library.with_suffix(".so")))]:
            run(["make", "-n", f"DOBBY_INCLUDE={header}", f"DOBBY_LIBRARY={archive}"], succeeds=False)
            run(["cmake", "-S", ".", "-B", str(Path(temporary) / ("cmake-" + label)),
                 f"-DDOBBY_INCLUDE={header}", f"-DDOBBY_LIBRARY={archive}"], succeeds=False)
            run(["meson", "setup", str(Path(temporary) / ("meson-" + label)),
                 f"-Ddobby_include={header}", f"-Ddobby_library={archive}"], succeeds=False)
    run(["make", "test-tools", "DOBBY_INCLUDE=", "DOBBY_LIBRARY="])
    run(["c++", "-std=c++23", "-O2", "-mbranch-protection=pac-ret+bti", "-I.",
         "-I" + str(include), "tests/function-hook-arm64.cpp", str(library), "-ldl",
         "-o", ".build/function-hook-arm64"])
    run([str(ROOT / ".build/function-hook-arm64")])
    run(["make", f"-j{args.jobs}", "OUT=.build/frontend-make.so",
         f"DOBBY_INCLUDE={include}", f"DOBBY_LIBRARY={library}"])
    run(["cmake", "-S", ".", "-B", ".build/cmake", "-DCMAKE_BUILD_TYPE=Release",
         f"-DDOBBY_INCLUDE={include}", f"-DDOBBY_LIBRARY={library}"])
    run(["cmake", "--build", ".build/cmake", "-j", str(args.jobs)])
    meson_args = ["meson", "setup"]
    if (ROOT / ".build/meson/meson-private/coredata.dat").is_file():
        meson_args.append("--reconfigure")
    run(meson_args + [".build/meson", f"-Ddobby_include={include}", f"-Ddobby_library={library}"])
    run(["meson", "compile", "-C", ".build/meson", "-j", str(args.jobs)])
    for binary in [".build/frontend-make.so", ".build/cmake/libspatialoverview.so",
                   ".build/meson/libspatialoverview.so"]:
        inspect(str(ROOT / binary))


if __name__ == "__main__":
    main()
