# Development notes

Things learned while making a PC emulator run well on the Tab5 (ESP32-P4: two RISC-V cores at up to 400 MHz, 32 MB
PSRAM, a small internal RAM, a 2D accelerator, an ESP32-C6 for Wi-Fi over SDIO). They describe what worked for this
firmware and why; they are observations from one project, not statements about the hardware or anyone's software.

## Two cores, two jobs

Core 1 runs the emulated PC and nothing else (`loop()`; its idle task isn't watched by the task watchdog, so it can
stay busy). Core 0 does everything else: the screen, sound, keyboard, touch, USB, Wi-Fi and the web server.

- **Input crosses cores through a queue.** The PC's keyboard and interrupt controllers belong to core 1. Changing
  them from core 0 while core 1 runs the PC can lose an interrupt, and with it the keyboard. Key and mouse events are
  queued and handed over by core 1 between its steps.
- **Pausing the PC is a counted hold**: whoever needs the PC stopped (the user, an open disk, the installer) takes
  a hold, and core 1 waits between steps while any is held.
- **A shared sound chip needs a lock.** The PC writes the OPL (AdLib) registers on core 1 while the sound task renders
  on core 0. Unsynchronised, that broke the synthesiser's channel state, not just a sample. A mutex around every use,
  with rendering in short pieces, keeps the PC from waiting long.

## Making the CPU fast

tiny386's interpreter decodes every instruction through general helpers: about 100 RISC-V instructions per x86
instruction. Two layers sit in front of it:

1. **A fast interpreter** (`t386_fast.h`) decodes the common instructions inline, turns memory operands into host
   pointers through a small TLB of its own, reads instruction bytes through a host pointer, and dispatches the next
   instruction from every handler. It works on tiny386's CPU state as it is, lazy flags included, so the two can take
   turns at any instruction boundary: anything unusual goes to tiny386 for that one instruction. Its handlers are
   compiled three times: for 32-bit and for 16-bit code with the operand and address sizes fixed (less code in the
   instruction cache, no size checks), and a generic copy for instructions with a 66 or 67 prefix. With the
   translator off, straight runs of instructions are also decoded once into **pre-decoded blocks** (`t386_fast_pd.h`)
   of small operations that run one after another, linked block to block; a block keeps a copy of its code and is
   compared again when something may have written it.
2. **A translator** (`t386_jit.h`) turns code that runs often into RISC-V code (real mode, virtual-8086 mode and
   protected mode, with or without paging), kept in a code cache in PSRAM. x86 registers live in RISC-V registers, flags stay lazy, a compare and a
   conditional jump become one branch, and blocks jump to each other directly.

Lessons:

- **Translate only what is hot.** Translating everything filled the cache over and over with code that ran a few
  times. Counting how often an address is reached, and translating at 8, works better.
- **Self-modifying code is common.** Programs patch immediates and displacements inside their inner loops. Detecting
  values that are rewritten soon after translation, and reading them from memory instead of baking them in, avoids
  translating the same block again and again.
- **New code must be made visible to instruction fetch.** After writing translated code through the data cache,
  the lines are written back to the shared L2 and the instruction cache's lines dropped (with fences), under the same
  lock the system's cache operations use. Each step is done twice, as ESP-IDF's own version of the ROM's write-back
  routine does: with one pass, translated code now and then ran as first emitted, before its jumps were patched
  (the occasional stops in long runs of programs that redraw the whole screen; caught with a debugger over the
  Tab5's USB port, a branch still pointing at itself while memory held it patched).
- **Single precision where it is exact.** The ESP32-P4 has a single-precision FPU. When the x87's precision control
  asks for single precision and the operands and result are normal singles, the FPU gives the x87's result; anything
  else (zeros, denormals, overflow) takes the exact software path.
- **Check against a reference.** The fast paths were checked instruction by instruction against tiny386's own
  interpreter, and the x87 against QEMU, on the PC. Most bugs showed up as a first differing register.

## The screen

- **Redraw only what changed.** About 30 times a second the VGA's registers, palette and visible memory are
  fingerprinted; nothing is drawn if they didn't change. The 2D accelerator (PPA) scales by 1.5 and rotates only the
  bands that differ.
- **Draw graphics modes at their own size.** Converting the 16- and 256-colour modes line by line at their native
  resolution, and letting the PPA scale them, took a redraw from ~40-65 ms to ~4-15 ms.
- **Snapshot the video memory.** A redraw on core 0 takes milliseconds while the program keeps drawing on core 1;
  reading a copy taken at one moment avoids half-old, half-new pictures.
- **Mind the memory bus.** Moving the whole picture every frame competed with the PC (and with the sound) for
  PSRAM; writing it in groups of lines, and only what changed, kept both smooth.
- **The vertical retrace is a clock, not a counter.** Programs poll the VGA's retrace bit; computing it from the time
  of each read is more faithful than stepping it from a task.

## Memory

- **Internal RAM is scarce once Wi-Fi is up** (the Wi-Fi link's buffers and network stack take ~160 KB). Large or
  long-lived buffers go to PSRAM: the code cache, guest RAM, LVGL's heap, TLS during the download, the installer
  task's stack, and (once Wi-Fi starts) any allocation of 512 bytes and more.
- **DMA from PSRAM needs whole cache lines.** Buffers the SD card's DMA reads must be cache-line aligned and written
  back from the cache first, or stale lines reach the card.
- **Don't hand constant data to a DMA write.** FatFs passes the caller's buffer straight to the card's driver for
  whole-sector writes. A string constant (in flash) given to one large `fwrite` failed with `sdmmc_write_blocks
  failed (0x102)`. Writing small files in pieces under a sector makes FatFs copy them through its own buffer.

## The SD card

- **Raw sectors for disk images.** Seeking backwards in a large file through FatFs walks its cluster chain from the
  start. Disk images are read and written as card sectors instead, with a read-ahead (DOS reads one sector at a
  time), through a list of where the image's pieces lie on the card, made at startup (and for a CD-ROM image when it
  is put in). Images the Tab5 writes (first-start install, web uploads) are put in one piece; one copied onto a card
  in use may lie in many.
- **Big card commands, the card working beside the PC.** A card command costs ~0.2 ms before any data moves, and a
  small write far more, so: a write command's sectors are gathered and written together when it ends (the BIOS
  sends a sector at a time); a read that misses takes 32 KB when it goes on from the last one; and during a
  sequential read, a task on core 0 reads the next 32 KB while the PC runs on core 1. The card reads ~19 MB/s at
  1 MB a command (40 MHz, 4 bits), 15.6 MB/s at 16 KB, 2.7 MB/s at 512 bytes (serial `sdbench`).
- **read(), not fread, for card files.** In ESP-IDF's C library a buffered `fread` from the card goes 512 bytes
  per card command (~1.1 MB/s) and an unbuffered one a byte per call (~0.05 MB/s); `read()` hands FatFs the whole
  request (~10 MB/s). Writes are ~4 MB/s either way.
- **The card and the Wi-Fi link share one SDMMC host.** While a card write holds the host, incoming network data
  waits in the Wi-Fi driver's few buffers. Uploads are received whole (256 KB at a time) into PSRAM before anything
  is written, so nothing arrives during the write.
- **Unbuffered images.** The PC's disk writes reach the card at once; the web file manager borrows the same file
  handles while the PC is paused, so there is never a second, stale view of a disk.

## The first start

Making an empty card turn into a working PC took more than downloading an installer:

- **Download in ranges, unzip as it comes.** The zip is fetched in 256 KB HTTP ranges and inflated with the ROM's
  `tinfl` straight onto the card, CRC checked; no temporary copy is needed.
- **Give the installer what it expects.** FreeDOS's USB installer wants a CD-ROM drive (an empty one will do) and
  installs to the second hard disk.
- **Partition and format C: in advance.** Within 1024 cylinders (504 MB) for older DOS versions. Installers then
  skip partitioning and the restart it needs, and one tested DOS installer CD only worked with an already formatted
  drive.
- **Boot the CD while the disk is empty.** Our own master boot record starts the partition only once its root
  directory has files; until then it hands over to the next boot device (the installer CD), also when an installer
  restarts the PC after formatting.
- **Finish the install for the user.** When the PC restarts with a system on C:, the Tab5 rewrites `tiny386.ini`,
  removes the installer's files, and adjusts `FDAUTO.BAT` (no CD driver without a CD drive, the right `BLASTER`).
- **Installs run with the translator.** For a while they ran without it: two of five installs with it on had
  stopped partway. That came from the code sync (see "New code must be made visible to instruction fetch"); since
  that fix, an install with it on copies in about 8 minutes.

## Working with the device

- **Look before typing.** Keys sent over serial before a program is ready go somewhere else; `tools/shot.py` first.
- **The USB serial port can renumber** after a reset (`/dev/ttyACM0` / `ACM1`); a long-running script should reopen
  it.
- **Screenshots over serial are checksummed**: while the PC is busy, pieces can go missing; `tools/shot.py` asks
  again.
- **Firmware panics leave a core dump in flash**, readable with ESP-IDF's `esp-coredump` and the build's ELF.
