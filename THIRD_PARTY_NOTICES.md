# Third-party notices

This project's own code and documentation are under the MIT License (`LICENSE`). This file lists the third-party
material: first what this repository itself contains, then what the tools fetch and the firmware is built from. Full
license texts for the material in this repository are in `LICENSES/`. The notices are reproduced from the upstream
sources at the versions given. This list was put together with care, but it is not legal advice.

## In this repository

| File(s) | What | License | Copyright | Text |
|---|---|---|---|---|
| `firmware/tab5_386/lv_conf.h` | LVGL's `lv_conf_template.h` (v9.5.0), with this project's settings | MIT | (c) 2025 LVGL Kft | `LICENSES/MIT-LVGL.txt` |
| `firmware/tab5_386/keyboard.h` (the key table `KBD_BASE`) | HID codes of the Tab5 Keyboard's keys, from [M5Unit-KEYBOARD](https://github.com/m5stack/M5Unit-KEYBOARD) | MIT | (c) 2025 M5Stack Technology CO LTD | `LICENSES/MIT-M5Unit-KEYBOARD.txt` |
| `firmware/tab5_386/src/t386_fast.h`, `t386_fast_ops.h`, `t386_fast_pd.h`, `t386_fast_pd_ops.h`, `t386_jit.h`, `t386_fpu_jit.h` | built on tiny386's internal definitions; `cpui386_step` adapted from tiny386's `i386.c` (those parts) | BSD-3-Clause | (c) 2024-2025 Chunhui He | `LICENSES/BSD-3-Clause-tiny386.txt` |
| `vendor/patches/0005`, `0006`, `0007`, `0009`-`0016` | changes to tiny386's `i386.c`, `fpu.c`, `pc.c` (with their code as context) | BSD-3-Clause | (c) 2024-2025 Chunhui He | `LICENSES/BSD-3-Clause-tiny386.txt` |
| `vendor/patches/0002`-`0004`, `0018` (`vga.c`), `0005` (`i8257.c`), `0007` (`i8254.c`), `0017` (`sb16.c`) | changes to tiny386 files from QEMU (with their code as context) | MIT | `vga.c`, `i8254.c`: (c) 2003-2017 / 2003-2004 Fabrice Bellard; `i8257.c`: (c) 2003-2004 Vassili Karpov (malc); `sb16.c`: (c) 2003-2005 Vassili Karpov (malc) | `LICENSES/MIT-tiny386-QEMU.txt` |
| `vendor/patches/esp-hosted/0001-sdio-rx-without-asserts.patch` | a change to ESP-Hosted's `sdio_drv.c` (with its code as context); it also adds a notice of the change to the file | Apache-2.0 | (c) 2024-2026 Espressif Systems (Shanghai) CO LTD | `LICENSES/Apache-2.0.txt` |
| `docs/images/fd-*.png`, `first-boot.png`, `help.png` | screenshots of FreeDOS 1.4 and its programs running in this emulator | the shown software's own (FreeDOS: GPL and other free licenses) | their authors | see [freedos.org](https://www.freedos.org/) |

This project's changes in the patch files are offered under the license of the file they change.

## Fetched by the tools and built into the firmware

None of this is kept in this repository. `tools/fetch_vendor.sh` fetches it at pinned versions, and the Arduino
toolchain provides the rest. A compiled firmware image contains all of it. The released images come with
`tab5-pc-<version>-NOTICES.txt` (all the notices and license texts) and `tab5-pc-<version>-source.tar.gz` (the
corresponding source, including the LGPL parts); see [`docs/DISTRIBUTION.md`](docs/DISTRIBUTION.md).

**[tiny386](https://github.com/hchunhui/tiny386)** at `vendor/tiny386.commit` (003c2666dfe6), with `vendor/patches`.
Its README states: "The cpu emulator and the project as a whole are both licensed under the BSD-3-Clause license.
Adlib emulation is an optional part of the project, and it requires the library fmopl which is licensed under the
LGPL. [...] SeaBIOS is distributed under the GNU LGPL-3 license. Some parts ported from QEMU/TinyEMU are under the
MIT license." The files the firmware compiles (through `firmware/tab5_386/src/t386_*.c`), by their own headers:

| tiny386 file | License | Copyright |
|---|---|---|
| `i386.c`, `fpu.c`, `pc.c`, `misc.c` and headers without a notice | BSD-3-Clause (tiny386's `LICENSE`) | (c) 2024-2025 Chunhui He |
| `fmopl.c`, `fmopl.h`, and `fmopl.inc` (generated from `fmopl.c`) | **LGPL-2.1-or-later** | (c) 1999, 2000 Tatsuyuki Satoh, MultiArcadeMachineEmurator development |
| `adlib.c`, `sb16.c`, `i8257.c` | MIT | (c) 2004-2005, 2003-2005, 2003-2004 Vassili Karpov (malc) |
| `vga.c`, `ide.c`, `i8042.c`, `i8254.c`, `i8259.c`, `ne2000.c`, `pci.c` | MIT | Fabrice Bellard (years in each file) |
| `pcspk.c` | MIT | (c) 2006 Joachim Henke |
| `ini.c`, `ini.h` (inih) | BSD-3-Clause | (c) 2009-2020 Ben Hoyt |
| `vgafont.c` (the VGA text font) | no notice in the file; provenance not established | |

**SeaBIOS and its VGA BIOS** (LGPL-3.0), built into the firmware as data (`tools/gen_bios.sh`). The images are
pinned in `vendor/bios.lock`: SeaBIOS rel-1.17.0-20-g81ec9ec0 (coreboot/seabios 81ec9ec0bcf4) with tiny386's
`seabios/patch` and `seabios/config`. `tools/build_seabios.sh` builds them from that source.

**[ESP-Hosted](https://github.com/espressif/esp-hosted-mcu)**, the SDIO host driver at `vendor/esp-hosted.commit`
(2.12.8), with `vendor/patches/esp-hosted`. Apache-2.0, (c) Espressif Systems (Shanghai) CO LTD.

**The Arduino toolchain** (the versions this project is built with):

- the M5Stack board package `m5stack:esp32` 3.3.9: the Arduino core for ESP32 (arduino-esp32, LGPL-2.1 as a whole,
  with files under other licenses) and ESP-IDF v5.5.4's precompiled libraries (Apache-2.0, with components under
  their own licenses: FreeRTOS, lwIP, mbedTLS, newlib, TinyUSB, ESP-Hosted, ...);
- [M5Unified](https://github.com/m5stack/M5Unified) 0.2.26 (MIT, (c) 2021 M5Stack);
- [M5GFX](https://github.com/m5stack/M5GFX) 0.2.36 (MIT, (c) 2021 M5Stack), which includes LovyanGFX (FreeBSD
  license) and other parts listed in its README; of its fonts the firmware uses the default one (Adafruit GFX's,
  BSD) and Font2 and Font4 (Bodmer's TFT_eSPI, FreeBSD license);
- [LVGL](https://github.com/lvgl/lvgl) 9.6.0 (MIT, (c) 2025 LVGL Kft), with its built-in Montserrat fonts (SIL Open
  Font License 1.1), symbol font (from Font Awesome Free) and UNSCII 8 (by Viznut, public domain), from which
  `tools/fonts/px_fonts.py` makes the settings panel's pixel fonts (`src/px_fonts.c`).

## Downloaded at run time

**FreeDOS 1.4** (`FD14-LiteUSB.zip`, from download.freedos.org) is downloaded by the Tab5 or by
`tools/make_sdcard.py`. Neither this repository nor the firmware contains it. Its packages are under the GNU GPL and
other free licenses, each package's own. Disk images made from it are a separate deliverable (see
`docs/DISTRIBUTION.md`).

## Trademarks

M5Stack and Tab5 are trademarks of their owner. FreeDOS and other names are trademarks of their respective owners.
This project is not affiliated with or endorsed by any of them.
