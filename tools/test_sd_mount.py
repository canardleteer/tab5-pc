#!/usr/bin/env python3
"""Check the production SD mount code with a host C++ compiler and fake SD/SPI APIs.

    python3 tools/test_sd_mount.py

No Arduino toolchain, vendor sources or device is needed. CXX selects the compiler.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parent.parent
    sketch = (root / "firmware/tab5_386/tab5_386.ino").read_text()
    config_start = sketch.index("#ifndef T386_SD_SPI\n")
    config_end = sketch.index("#if T386_SD_SPI\n", config_start)
    mount_start = sketch.index("#if T386_SD_SPI\n// Tab5 microSD SPI wiring")
    mount_end = sketch.index("\nstatic bool file_exists(", mount_start)
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    flags = ["-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror"]
    with tempfile.TemporaryDirectory(prefix="tab5-sd-mount-") as directory:
        work = Path(directory)
        # Compile the configuration and mount function from the sketch, without copying their logic into the test.
        (work / "sd_transport_config.h").write_text(sketch[config_start:config_end])
        (work / "production_sd_mount.h").write_text(sketch[mount_start:mount_end])
        for name, definitions in [("default", []), ("sdmmc", ["-DT386_SD_SPI=0"]), ("spi", ["-DT386_SD_SPI=1"])]:
            binary = work / name
            subprocess.run(compiler + flags + definitions + ["-I" + str(work),
                           str(root / "tools/tests/sd_mount.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
    print("SD mount: default SDMMC, explicit SDMMC and SPI checks passed")


if __name__ == "__main__":
    main()
