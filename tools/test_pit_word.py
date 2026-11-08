#!/usr/bin/env python3
"""Run the PIT word-input regression with a host C compiler and the fetched tiny386 source.

    python3 tools/test_pit_word.py

Run tools/fetch_vendor.sh first. No Arduino toolchain, QEMU installation or device is needed.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parent.parent
    vendor = root / "vendor/tiny386"
    if not (vendor / "i8254.c").is_file():
        raise SystemExit("run tools/fetch_vendor.sh first")
    compiler = shlex.split(os.environ.get("CC", "cc"))
    flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I" + str(vendor)]
    with tempfile.TemporaryDirectory(prefix="tab5-pit-") as directory:
        work = Path(directory)
        # The vendor model has an unused parameter; keep warnings strict for the adapter and test.
        subprocess.run(compiler + flags + ["-Wno-unused-parameter", "-c", str(vendor / "i8254.c"),
                                           "-o", str(work / "i8254.o")], check=True)
        subprocess.run(compiler + flags + ["-I" + str(root / "firmware/tab5_386/src"),
                                           str(root / "tools/tests/pit_word.c"), str(work / "i8254.o"),
                                           "-o", str(work / "pit_word")], check=True)
        subprocess.run([str(work / "pit_word")], check=True)


if __name__ == "__main__":
    main()
