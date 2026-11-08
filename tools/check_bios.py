#!/usr/bin/env python3
"""Check the BIOS images in vendor/ (bios.bin, vgabios.bin) against the pinned ones in vendor/bios.lock.

An image passes if it is the tested image byte for byte, or the same build of the same source apart from its build
stamp: SeaBIOS writes the build's date and host into its version string (rel-...-YYYYMMDD_HHMMSS-host), and the VGA
BIOS's option ROM checksum (byte 6) follows from it. tiny386's continuous release is rebuilt with each upstream push,
so its images change in those bytes alone while the SeaBIOS source stays the same.

    tools/check_bios.py [--built NAME]... [DIR]      (default: vendor/)
--built NAME: that image was built here (tools/build_seabios.sh): another compiler's bytes differ from the tested
build's, so it passes if its SeaBIOS version is the pinned one.
"""
import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STAMP = re.compile(rb"(rel-[0-9][0-9.]*-[0-9]+-g[0-9a-f]+(?:-dirty)?)(-[0-9]{8}_[0-9]{6}-[\x21-\x7e]*)")


def version(data):
    m = STAMP.search(data)
    return m.group(1).decode() if m else None


def unstamped(data):
    d = bytearray(data)
    for m in STAMP.finditer(data):
        d[m.start(2):m.end(2)] = bytes(m.end(2) - m.start(2))
    if d[:2] == b"\x55\xaa":   # an option ROM: its checksum byte
        d[6] = 0
    return bytes(d)


def main():
    args, built = sys.argv[1:], set()
    while len(args) >= 2 and args[0] == "--built":
        built.add(args[1])
        args = args[2:]
    folder = args[0] if args else os.path.join(ROOT, "vendor")
    pins, want_version = {}, None
    for line in open(os.path.join(ROOT, "vendor", "bios.lock")):
        f = line.split()
        if not f or f[0].startswith("#"):
            continue
        if f[0] == "version":
            want_version = f[1]
        elif f[0].endswith(".bin"):
            pins[f[0]] = (f[1], f[2])
    ok = True
    for name, (exact, blanked) in pins.items():
        data = open(os.path.join(folder, name), "rb").read()
        if hashlib.sha256(data).hexdigest() == exact:
            print(f"{name}: the pinned image ({version(data)})")
        elif hashlib.sha256(unstamped(data)).hexdigest() == blanked:
            print(f"{name}: the pinned build ({version(data)}), with another build stamp")
        elif name in built and version(data) == want_version:
            print(f"{name}: built here from the pinned source ({version(data)}; another compiler than the tested build's)")
        else:
            print(f"{name}: not the pinned image (SeaBIOS {version(data)}; pinned: {want_version})", file=sys.stderr)
            ok = False
    if not ok:
        sys.exit("The BIOS images differ from those in vendor/bios.lock. Images built here by tools/build_seabios.sh\n"
                 "always do (another compiler); otherwise tiny386's continuous release may have moved on: build them\n"
                 "from the pinned source with BIOS_FROM_SOURCE=1 tools/fetch_vendor.sh, and test the firmware again.")


if __name__ == "__main__":
    main()
