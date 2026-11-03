# Changelog

## Unreleased

- Paused, the PC's picture is dimmed (each colour halved) and left as it was, a text mode's cursor no longer
  blinking; the backlight stays at the brightness setting (it was a quarter of it, the side panels included). The
  picture comes back in full when the PC goes on.

## 1.4.0 (2026-11-02)

A CD-ROM tray on the side panel (the disc changes while the PC runs), and fixes: the keyboard and mouse after RESET,
a DOS extender's real-mode calls, the DMA controller's decrement mode, serial checks for the speaker. The firmware
image with its notices and corresponding source, as for 1.3.0.

- A PC reset (a reboot, the RESET button) resets the keyboard controller, the keyboard and the mouse too, as a real
  board's reset does. After RESET, a mouse moved while Linux started left it with no keyboard and no mouse: the mouse
  was still reporting from before the reset, its data piled up, and Linux, finding more than 16 bytes waiting, took
  the controller for absent ("i8042: No controller found") (vendor/patches/0022).
- A CD-ROM tray on the right side panel, between the lights and the battery: the disc's name; a tap opens a dialog
  with the `.iso` files at the card's root and "No disc", **Insert** / **Eject**. The disc changes while the PC runs
  (the PC is told the medium changed, as by a real drive) and is saved in the machine's settings file (its `cdX`
  line), so it stays in the drive across restarts. The right panel's three windows (lights, CD tray, battery) are
  one size. Every machine has a CD-ROM drive now: its settings file's, or an empty one as `cdc` (`cdd` if `hdc` is
  taken); a disc not on the card leaves the drive empty instead of removing it. FreeDOS installs keep their CD-ROM
  driver (`CDROM.BAT` no longer gets a `rem`). Serial `cd [<file>.iso | -]`. tiny386's drive gets empty drives,
  "medium not present" with no disc, and a medium-change notice reported once (FreeDOS's CD-ROM driver never asks
  for it, and the drive stayed "not ready") (vendor/patches/0021).
- A real-mode interrupt (hardware ones too) changes SP only, as on x86: it set ESP to (ESP - 6) & 0xffff, clearing
  ESP's upper half, which a DOS extender's real-mode calls can keep a value in (vendor/patches/0019; sent to
  tiny386 upstream with the AAM, 9th-column and Sound Blaster DMA fixes).
- The DMA controller's decrement mode transfers downwards from the current address, as the 8237: reads overwrote
  half their bytes instead of reversing them, and with the Sound Blaster's shorter reads (1.3.0) a sound played that
  way would have come out wrong; writes scrambled the buffer they came from (vendor/patches/0020, also upstream).
- Serial `S` shows `peak`, the loudest sample the PC played since the last `S`; serial `beep` plays a tone through
  the speaker without the PC and shows the amplifier's enable bit. If the PC plays (a high peak) but nothing is heard,
  even the beep: switching the Tab5 fully off and on again has brought the speaker back (restarts leave its codec and
  amplifier powered).

## 1.3.0 (2026-11-01)

Side panels beside the PC's picture (buttons and lights, as on a PC's case), ISO installs, a faster translator with a
larger code cache, and fixes (FreeDOS EDIT under JemmEx, the card under Wi-Fi traffic, the speaker, thin lines across
pictures). The firmware image with its notices and corresponding source, as for 1.2.1.

- **Side panels** beside the PC's picture, as on a PC's case: POWER (held a second: the PC off, its picture gone and
  its sound stopped; a tap: on again, a cold start), RESET (held a second), PAUSE (the backlight dimmed, the sound
  stopped) and SETUP (the settings panel) on the left; the power light, the disk light and the battery on the right.
  A ring fills around POWER's or RESET's icon while held; short taps do nothing while the PC runs. Drawn with LVGL
  (anti-aliased) as two displays of their own, only where something changed: programs run as fast as without them.
- The PC's picture is always 4:3 (960 x 720 at most); the settings panel's picture-shape switch is now **Side
  panels** (off: black margins, part of the touchpad).
- The PC off is a state of its own: when its system powers it off (APM) the firmware no longer stops for good, and
  POWER turns it on again. Paused or off, the sound is silent (a held note went on before).
- Battery: a pack counts as present only after a minute without a low reading (with none fitted, the charger's cycle
  showed a full pack for a moment now and then).
- The translator brings dead translations back: each block keeps the x86 bytes it was translated from, and when the
  same code is at the same place again (a program loaded where it was before, code a program patches and puts back)
  the old translation is used instead of a new one. A dead block's entry jumps out while the links into it stay, for
  when it lives again; the links of dead blocks are used again; a large write cleans up its pages once. Installing
  FreeDOS (its command shell swaps itself out and back for each program the installer runs, ~4500 blocks a second
  died and were translated again, the code cache emptied every 5 s): the copy 578 -> 495 s.
- A short conditional jump whose opcode a program rewrites as it runs (a game's ray caster, for every ray) reads it
  where it runs instead of being translated again; writes to code that invalidate nothing skip the search for blocks.
  That game: 40-41 -> 42-46 fps.
- 16-bit addresses that are a constant alone take 1 or 2 instructions instead of 5.
- With paging, translations ~30% smaller (70 -> 49 bytes an x86 instruction): a read's or write's slow path, pushes
  and pops through shared routines. A graphical desktop starts 4% faster and its applications 7-12%; a Linux
  workload 10-16%. The indirect jump cache's slot by a multiplicative hash: Linux's console prints 5-7% faster.
- Sound Blaster: its DMA reads only what fits in its buffer (it read up to 512 bytes and dropped them while the buffer
  was full: up to 3% of the time while digitized sound played). vendor/patches/0017.
- The installers download in 256 KB pieces, not 16 KB (each piece a round trip to the server): FreeDOS 1.4's 17 MB
  in ~40 s instead of ~5.5 min; a Sprig Linux install, all of it, in under a minute.
- Serial `text`: the screen's text in a text mode; `stap <x> <y> <ms>`: a press on the side panels (for tests).
- The card's file system sectors go through a buffer of the firmware's own in PSRAM: left to the card's driver, each
  took a little DMA-capable internal RAM, of which Wi-Fi leaves only ~3 KB, and with traffic on the web page that
  ran out (a file not created, a folder listed empty, as if the card had gone). An upload refused unread (over
  256 KB) closes its connection (its data held Wi-Fi's receive buffers). The code cache leaves the PSRAM meant for
  Wi-Fi, uploads and the disks again (the revival tables were left out of its sizing).
- The speaker starts once, when the Tab5 starts, and stays; silence switches only its amplifier off. Started with
  each sound, it took a little DMA-capable internal RAM each time, which Wi-Fi can leave too little of: the sound
  failed to start and came out as noise (a graphical desktop's start and exit sounds, with Wi-Fi on).
- No more thin dark lines left across a picture drawn from the bottom up (a graphical desktop's startup logo, about 2
  starts in 3): the PPA's scaling filter mixes the line above a group of 16 picture lines into the group's first
  panel line (and the line below into its last), so a group is drawn again when the line next to it changed, not
  only its own lines.
- The translator's code cache takes the PSRAM free when the PC starts, less 1.5 MB kept for Wi-Fi, uploads and
  disks: 3 to 8 MB instead of 3 MB (about 5.5 MB for a PC with 16 MB of RAM), with a block in its tables per 256
  bytes of it. Programs with a lot of code empty it less often: a graphical desktop's applications start 15 to 20%
  faster (programs whose code fitted in 3 MB run as before, within 1%). Room for it: M5GFX's second frame buffer,
  which the panel never shows, is given back (1.8 MB); the settings panel draws with one buffer instead of two (it
  waited for each anyway); the translator's page map takes 128 KB instead of 1 MB (the MMU maps its part for
  addresses above 256 MB, all "not RAM", onto one shared page).
- The translator's two biggest functions are compiled for size: its code runs from flash, and translating mostly
  waits for it there. A block takes about 10% less time to translate (Linux starting: 47 -> 43 us a block).
- The Machine frame's **ISO**, beside **DOS** and **Linux** (as the three now read): a new machine that starts from
  an installer CD's `.iso` at the card's root, chosen in the dialog, with an empty 512 MB `C.IMG` (no download, no
  Wi-Fi needed); it starts from C: once the installer has put a system there. Serial: `install iso <name> [<file>]`.
- README: no longer says the translator is off while an installer runs (it has been on since 1.1.0).
- The translator makes fewer moves: MOV between 32-bit registers as one move (was two); a shift of a 32-bit register
  by a constant whose flags aren't read, as one instruction (was three); an address [base+index] without a
  displacement without an extra add.
- REP MOVS and REP STOS are translated: when both sides are plain RAM (the destination without translated code),
  each within a page, forwards, the copy or fill is one call (memcpy, or element by element where the destination
  overlaps the source after it, as the CPU does); with paging each side's page comes from the fast path's TLB. Before,
  every one went to the interpreter. 32-bit DIV and IDIV of a 64-bit EDX:EAX (16.16 fixed-point divides) are
  translated too (a zero divisor or a quotient that doesn't fit: #DE from the interpreter, as before); they left the
  block every time. A 3D game's timed demo ~5% faster; Linux's gunzip ~4%.
- The instructions run in place are counted by opcode only once asked for (the count cost every one a little).
- Side panels: the power and disk lights keep their colour, dim, when off, as a real LED's lens; the battery icon is
  grey (red at 15% or less), no longer green.
- The settings panel's dialog (a Wi-Fi password, a new machine's name) is as tall as what it shows: ISO's buttons
  ran past its bottom edge, its title and hint past its right edge. Titles "Install DOS / Linux / ISO: machine name".
- Fix: a translated far JMP or CALL in virtual-8086 mode with paging (DOS under a memory manager such as JemmEx or
  EMM386), to code in another page, went on with the old code segment's base and ran whatever was translated at
  that offset in the calling segment. FreeDOS's EDIT under JemmEx (the FreeDOS installer's default) stopped with
  "exception 06" almost every time it started after the first; now 8 starts of 8 run. (Since 1.0.0.)
- Text modes are drawn 8 dots wide by default (640 x 400, scaled 1.5 to the panel) instead of 9 (720 x 400, scaled
  4/3): shaded backgrounds (FreeDOS EDIT's desktop) showed vertical stripes. `vga_force_8dm = 0` in a machine's ini
  brings 9-dot cells back. With 9-dot cells, only the box-drawing characters C0h-DFh repeat their 8th column in the
  9th, as on the VGA; the shades B0h-B2h no longer get a doubled dot in every cell (vendor/patches/0018).
- Side panels: POWER or RESET held, the settings panel opened by a key before the second was up and the finger lifted
  while it was open: the hold fired once the panel closed (the PC off or reset). Opening the panel now forgets the
  press. Pause and Resume in the settings panel silence and restore the sound at once (Resume used to leave the PC
  silent until the panel closed, Pause to leave held notes playing), and the panel has its full brightness when
  opened while the PC is paused. Serial `S` shows "(silenced)" and the backlight.

## 1.2.1 (2026-10-28)

The empty card's settings panel with the Machine frame alone, and README updates. The firmware image with its
notices and corresponding source, as for 1.2.0.

- The settings panel on an empty card: no FreeDOS frame in place of the sliders any more; the Machine frame's
  **Install DOS** and **Install Linux** set up the first machine, as any other (its list reads "(none yet)"). The
  offer to start the PC from an installer CD's `.iso` at the card's root went with the frame (the README says how to
  set that up by hand); a machine set up that way by 1.2.0 still finishes its install as before.
- README: a screenshot of Sprig Linux starting; the empty card's panel.

## 1.2.0 (2026-10-28)

Machines by name, with DOS and Linux installed from the settings panel; a faster fast interpreter; fixes. The firmware
image with its notices and corresponding source, as for 1.1.0.

- `LICENSE` holds the plain MIT text (the list of third-party material is in `THIRD_PARTY_NOTICES.md`).
- Machines: the settings panel's **Machine** frame lists `default` (`/tiny386.ini`) and the machines named N
  (`/tiny386-N.ini`, their files in `/N`), and restarts into the one chosen, which stays chosen through every restart.
- **Install DOS** and **Install Linux** make a named machine: FreeDOS 1.4 (its own installer), or the latest
  [Sprig Linux](https://github.com/yejun/sprig-linux) release, every file checked against its checksums (installing
  it again with the same name updates its kernel, keeping its files). The Tab5 restarts into an install mode for it,
  with no PC running (all the memory free), then into the new machine. The empty card's **Install FreeDOS 1.4** asks
  for a name too.
- The fast interpreter (used with the translator off, and by the translator for what it leaves) is about 1.5 times
  as fast: its handlers in 16-bit, 32-bit and generic copies, cheaper memory and window checks, and pre-decoded
  blocks (`t386_fast_pd.h`; serial `pd`, `pd 0|1`), kept in the translator's code cache while it is off. A game's
  built-in benchmark with the translator off takes about 35% less time.
- A program that goes wild no longer takes the Tab5 down: an interrupt or exception that can't be delivered becomes
  a double fault, and if that fails too, a PC reset (a triple fault, as a board does); tiny386 aborted there.
- AAM with an immediate of 0 raises a divide error (`vendor/patches/0016`); it divided by 0.
- Fast interpreter fixes: MOVS from video memory with a null ES raised no #GP; a real-mode INT whose stack overlapped
  the vector table read the vector after pushing; exceptions in its handlers left the instructions before them out of
  the instruction count; pre-decoded blocks at addresses 0-2 missed stores into them, and code going to another page
  at every block (a loop over a page boundary with paging) didn't return to service devices.

## 1.1.0 (2026-10-26)

The PC's network, a faster translator (Linux too), VBE graphics modes, a DOS-era settings panel and web page, and
fixes. Firmware images with their notices and corresponding source, as for 1.0.0.

- The PC's network card (NE2000, port 300h, IRQ 9) is connected while Wi-Fi is on: a network of its own inside the
  Tab5 (192.168.77.0/24, the Tab5 at .1 with a DHCP server), routed to the Wi-Fi network with address translation;
  ports 2222, 2323 and 8080 of the Tab5 lead to the PC's 22, 23 and 80. Serial `net` shows the card's state.
- One lock for the SD card: the PC's disks (raw access) and FatFs (the web page, the settings panel) each had their
  own, and a command sent while the card was busy with the other's could time out, so a web download could end
  early. Downloads that fail now close the connection (the browser reports an error, not a shorter file).
- The translator: more instructions translated (SHLD/SHRD by CL and in memory, BSF/BSR, BT/BTS/BTR/BTC in memory
  with the bit number in a register, BSWAP, RDTSC, CMPXCHG, XADD, FWAIT); a bigger table of translated blocks; in
  protected mode with DS, ES and SS based at 0 (flat), their bases aren't added to every address. The fast
  interpreter computes the carry flag inline.
- tiny386 fixes (`vendor/patches`): CMPXCHG set the flags of destination - accumulator instead of accumulator -
  destination (0014); 64-bit integers and 80-bit values copied through the FPU (FILD/FISTP, FLD/FSTP) lost bits
  beyond a double's 53 (0015).
- The translator's description (README, `docs/NOTES.md`): it handles paged protected mode too.
- The PC's picture at 4:3, as a monitor showed every mode (the default): text, 320 x 200 and 640 x 350 graphics fill
  the panel's height like 640 x 480 (at most 960 x 720). A switch in the settings panel, or serial `aspect 43|square`,
  goes back to square pixels. Fixed with it: the last lines of a picture whose height isn't a multiple of 16
  (640 x 350) weren't redrawn, and the black around a picture was cleared on the wrong side upside down.
- tiny386 at upstream 1a8f1e0 (was d81e225), which has the task-switch and timer-mode fixes (patch 0011 and
  `t386_pit_fix.h` dropped) and VGA, PS/2, ATAPI and floppy fixes; patches 0002, 0004 and 0007 rebased onto it.
- Disk images that lie in pieces on the card (copied onto a card in use) are read and written as card sectors too,
  through a list of their pieces made at startup, instead of through FatFs, which walked the file's cluster chain on
  every backward seek. Reading a 3 MB file from an image in 590 pieces took 2.6 s instead of 85 s.
- Video memory in the VGA's other write modes is written without tiny386's callbacks: write mode 0 with rotation,
  set/reset, a logical operation or a bit mask, and write modes 2 and 3, by translated code itself, and read mode 1
  (colour compare) by the fast path. Twenty fills of the 640 x 480 x 16 screen with set/reset or write modes 2 and 3
  take about 6-7 BIOS ticks instead of 16-17. Writes go nowhere while the misc output register's RAM enable bit is
  clear, as in vga.c.
- VBE modes up to 1024 x 768 (were 720 x 480): 800 x 600 and 1024 x 768 with 256 colours, 32K and 64K, 800 x 600
  with 16M too (2 MB of video memory), drawn at their own size and scaled onto the panel by the PPA (1024 x 768 at
  960 x 720). tiny386's renderer cut off what lay beyond 720 x 480. The 1024 x 768 buffer (1.5 MB) is taken when
  there is the memory, else VBE modes stay at 720 x 480.
- What VBE modes' writes change (through the linear frame buffer or the 64 KB window) is marked in 2 KB pieces, and
  only those lines are read again: checking a still 1024 x 768 x 16 picture for changes takes 0.1 ms a frame instead
  of 15 ms.
- The copy of video memory each tiny386 refresh reads is 256 KB, not all of it: 1.3 MB more memory free.
- tiny386.ini [pc] ne2000_irq: the network card's interrupt (9 unless set; 3, 5, 7, 10, 11 or 15). Some NE2000 drivers
  don't work on 9, the second interrupt controller's line that stands in for 2.
- The linear frame buffer is mapped by the fast path's TLB straight to video memory, as RAM is: reads by translated
  code and the fast path at RAM speed, writes without a page walk and tiny386's callbacks (still marked for the
  screen). Its place comes from pc.c's BAR callback.
- The settings panel and the web page in a DOS-era look: the panel like a BIOS setup screen in a pixel font drawn
  at 2x3 and 3x4 (UNSCII), the web page like a DOS file manager.
- The settings panel's Wi-Fi switch moves as soon as it is tapped (it waited for Wi-Fi to start, a few seconds),
  with "Switching Wi-Fi on ..." meanwhile; its label toggles it too, and the switches take taps around them. After
  switching Wi-Fi on, the networks are scanned once it has connected (the list said to switch Wi-Fi on).
- A 2D engine for the linear frame buffer on port F1E0h (fill, copy, monochrome expansion, `src/t386_accel.h`), for
  a Linux guest whose vesafb uses it: its graphics console prints twice as fast.
- Linear frame buffer writes: a page is marked for the screen once per refresh, at its first write, then written at
  RAM speed until the next refresh (before, every write went through the TLB's slow path to mark its piece).
- The translator: jumps between blocks in different pages, and blocks whose last instruction runs into the next
  page, are linked (serial `jit xlink 0` turns that off). Translated code leaves for the dispatcher
  when tiny386 empties its TLB in place (INVLPG).
- tiny386 at upstream 003c266 (was 1a8f1e0): FXCH without losing precision, VGA line compare and pel panning,
  linuxstart's `vga=` option; patches 0002, 0013 and 0015 rebased (0015 now only the 80-bit moves).
- The translator's code sync writes back and drops cache lines twice, as ESP-IDF's own version of the write-back
  routine does (this build linked the ROM's). With one pass, new code occasionally ran as first emitted, before its
  jumps were patched: the rare stops in long runs of programs that redraw the whole screen. A 4-minute game demo 20
  times over and a DOS stress loop for 44 minutes ran without one (before, a stop within a few runs).
- FreeDOS's installer runs with the translator on again (it ran with it off after two of five installs had stopped
  partway, from the code sync above): copying took about 8 minutes.
- Restarts (the web page's, the installer's) no longer lose the SD card now and then: after a restart with the PC
  running from the card until just before, the card was sometimes missing (the PC started without disks) or the
  start hung, until the Tab5 was switched off; 3 in 30 restarts. The Tab5 now restarts through deep sleep, a fuller
  reset (30 of 30 found the card and Wi-Fi), and once more that way if the card is missing after a panic.
- Settings panel: the battery's state fits its title bar ("Charging 82% - ~0 h 37 min to full"); it took two lines,
  the second cut off. Volts and milliamps stay in the serial and web status.
- `tools/fetch_vendor.sh` applies ESP-Hosted's patch in a clone of this repository too: run inside another
  repository's work tree, `git apply` skipped the file and still reported success, so a build from a clone had the
  Wi-Fi link's driver unpatched (the 1.0.0 images were not affected).
- The PC's network is there from the start, Wi-Fi on or off (it started with Wi-Fi): with Wi-Fi off the PC still gets
  its address by DHCP, instead of its DHCP client waiting for a server that wasn't there (some systems stopped at a
  message about it). The DNS server handed out is now the Tab5 itself, relaying to the Wi-Fi network's DNS server of
  the moment, so names resolve on a lease taken before Wi-Fi was on or before it joined another network; with Wi-Fi
  off, lookups fail at once. Network setup failures leave the card unconnected instead of half set up.
- Serial `ptap x y` (a tap on the settings panel) and `mev dx dy buttons` (a PS/2 mouse event), for testing.

## 1.0.0 (2026-10-22)

The first public release: the source code, and firmware images with their notices and corresponding source (see
`docs/DISTRIBUTION.md`). No SD card images.

- A 386/486 PC on the M5Stack Tab5, built on tiny386 (pinned commit, with the patches in `vendor/patches`): VGA,
  PS/2 keyboard and mouse, Sound Blaster 16, AdLib, PC speaker, IDE hard disks, CD-ROM images, floppies, SeaBIOS.
- A fast interpreter and a translator to RISC-V code in front of tiny386's own CPU (see `docs/NOTES.md`).
- First start on an empty card: downloads FreeDOS 1.4 and runs its installer, or starts an installer CD image from
  the card; then sets the PC up to start from `C.IMG`. `tools/make_sdcard.py` prepares a card on a computer instead.
- The Tab5 Keyboard, USB keyboards and mice, and the touch screen as a touchpad.
- Copying files over Wi-Fi from a browser (the SD card and the PC's disks); `tools/upload.py` from a shell.
- The settings panel: Wi-Fi, brightness, volume, touchpad speed, pause and reset.
- The serial console: status, screenshots (`tools/shot.py`), commands.
