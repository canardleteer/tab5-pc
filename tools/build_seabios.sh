#!/usr/bin/env bash
# Build the BIOS images (vendor/bios.bin, vendor/vgabios.bin; BIOS_ONLY=1: bios.bin alone) from source: SeaBIOS at the commit in vendor/bios.lock
# with tiny386's seabios/patch and seabios/config (from vendor/tiny386, tools/fetch_vendor.sh), as tiny386's
# scripts/build.sh does. Needs git, make, python3 and a gcc that can build 32-bit x86 code (gcc -m32).
#
# The result is built from the same source as the pinned images, but with this computer's compiler, so its bytes
# differ from theirs (tools/check_bios.py reports that). SeaBIOS writes the build's date and a host name (here "build",
# not this computer's) into its version string, which the firmware prints at start.
set -euo pipefail
cd "$(dirname "$0")/../vendor"
[[ -f tiny386/seabios/patch ]] || { echo "run tools/fetch_vendor.sh first" >&2; exit 1; }
SEABIOS=$(awk '$1 == "seabios" { print $2 }' bios.lock)
rm -rf seabios
git clone -q https://github.com/coreboot/seabios.git seabios
git -C seabios checkout -q "$SEABIOS"
# (SeaBIOS's version string takes the host name: "build" in its place, through a wrapper for its scripts' python)
cat > seabios/pyhost <<'PY'
#!/usr/bin/env bash
exec python3 -c 'import os, runpy, socket, sys; socket.gethostname = lambda: "build"; sys.argv = sys.argv[1:]
sys.path.insert(0, os.path.dirname(os.path.abspath(sys.argv[0]))); runpy.run_path(sys.argv[0], run_name="__main__")' "$@"
PY
chmod +x seabios/pyhost
(cd seabios && patch -s -p1 < ../tiny386/seabios/patch && cp ../tiny386/seabios/config .config &&
 make -s oldconfig PYTHON=./pyhost >/dev/null && make -s -j"$(nproc 2>/dev/null || echo 4)" PYTHON=./pyhost)
# each image with NAME.src: the fingerprint of the source it is built from (tools/check_bios.py --built checks it)
FP=$(python3 ../tools/check_bios.py --fingerprint)
if [[ "${BIOS_ONLY:-0}" == 1 ]]; then   # (tools/fetch_vendor.sh: the VGA BIOS from tiny386's release)
  cp seabios/out/bios.bin . && echo "$FP" > bios.bin.src
  echo "built bios.bin from SeaBIOS $SEABIOS"
else
  cp seabios/out/bios.bin seabios/out/vgabios.bin . && echo "$FP" > bios.bin.src && echo "$FP" > vgabios.bin.src
  echo "built bios.bin, vgabios.bin from SeaBIOS $SEABIOS"
fi
