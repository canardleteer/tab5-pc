// tiny386 core: pc.c (see tab5_config.h)
#include "tab5_config.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

// pc.c reports every access to an I/O port it doesn't emulate with fprintf(stderr): here a 115200 baud UART that
// nobody reads, and a program that keeps using such a port (a joystick, say) would spend its time waiting for it.
// They are counted instead, the latest message kept (serial "cpu"). Other messages (at startup) go to stderr.
static volatile unsigned t386_port_count;
static char t386_port_last[48];

static int t386_fprintf(FILE* f, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n;
    if (f == stderr && (!strncmp(fmt, "out", 3) || !strncmp(fmt, "in", 2))) {
        n = vsnprintf(t386_port_last, sizeof(t386_port_last), fmt, ap);
        ++t386_port_count;
    } else {
        n = vfprintf(f, fmt, ap);
    }
    va_end(ap);
    return n;
}

void t386_port_report(char* buf, int len) {
    int n = snprintf(buf, len, "unemulated port accesses: %u, the last: %s", t386_port_count, t386_port_last);
    if (n > 0 && n < len && buf[n - 1] == '\n') buf[n - 1] = 0;
}

// Instructions per pc_step: tiny386's ESP32 default is 512, and the devices are polled (PIT, CMOS, keyboard, DMA)
// between steps, at a cost of a few hundred instructions' time. The PIT raises at most one interrupt per poll, so
// steps stay short while channel 0 runs fast (sampled sound through the PC speaker: up to ~16 kHz), and get up to
// t386_step_max long (serial "steps <n>") at the usual rates (18.2 Hz to ~1 kHz).
#include "i8254.h"
int t386_step_max = 2048;
static inline int t386_step_count(PITState* pit) {
    const int n = pit_get_initial_count(pit, 0) * 2;
    return n < 512 ? 512 : n > t386_step_max ? t386_step_max : n;
}
#define PC_STEP_COUNT t386_step_count(pc->pit)

// The VGA window's accesses (pc.c's iomem callbacks, reached from tiny386's own interpreter and string
// instructions) try t386_vga_ext.h's fast paths before vga.c's.
#include "vga.h"
int t386_vga_write_fast(uint32_t off, int size, uint32_t v);
int t386_vga_read_fast(uint32_t off, int size, uint32_t* out);
static inline uint8_t t386_vga_mem_read(VGAState* s, uint32_t off) {
    uint32_t v;
    return t386_vga_read_fast(off, 1, &v) ? v : vga_mem_read(s, off);
}
void t386_vga_written(uint32_t off, int size);   // (t386_vga_ext.h: VBE banks)
#define T386_VGA_WRITE(f, size) \
    static inline void t386_##f(VGAState* s, uint32_t off, uint32_t v) { \
        if (!t386_vga_write_fast(off, size, v)) { f(s, off, v); t386_vga_written(off, size); } \
    }
T386_VGA_WRITE(vga_mem_write, 1)
T386_VGA_WRITE(vga_mem_write16, 2)
T386_VGA_WRITE(vga_mem_write32, 4)
// ... and its registers' writes keep the translator's write mode (t386_vga_fw.h) up to date.
#include "t386_vga_fw.h"
static inline void t386_vga_ioport_write(VGAState* s, uint32_t addr, uint32_t val) {
    vga_ioport_write(s, addr, val);
    t386_vga_fw_update();
}
static inline void t386_vbe_write(VGAState* s, uint32_t offset, uint32_t val) {
    vbe_write(s, offset, val);
    t386_vga_fw_update();
    t386_vram_mark(0, 1u << 31);   // (a mode set clears video memory)
}
// The vertical retrace (status register 3DA/3BA) as of the moment it is read: vga_step moves it on by the time. (Core 0
// did it every millisecond, but not while it draws the screen, 30-80 ms: a program could wait for it that long.)
int t386_vga_step(VGAState* s);   // (t386_vga_ext.h)
static inline uint32_t t386_vga_ioport_read(VGAState* s, uint32_t addr) {
    if (addr == 0x3da || addr == 0x3ba) t386_vga_step(s);
    return vga_ioport_read(s, addr);
}
#define vga_ioport_write t386_vga_ioport_write
#define vga_ioport_read t386_vga_ioport_read
#define vbe_write t386_vbe_write
uint32_t t386_vbe_read(VGAState* s, uint32_t offset);   // (t386_vga_ext.h: the largest VBE mode)
#define vbe_read t386_vbe_read
#define vga_mem_read t386_vga_mem_read
#define vga_mem_write t386_vga_mem_write
#define vga_mem_write16 t386_vga_mem_write16
#define vga_mem_write32 t386_vga_mem_write32
bool t386_vga_text_write_string(uint32_t off, const uint8_t* buf, int len);   // (t386_vga_ext.h: odd/even modes)
static inline bool t386_vga_mem_write_string(VGAState* s, uint32_t off, uint8_t* buf, int len) {
    return t386_vga_text_write_string(off, buf, len) || vga_mem_write_string(s, off, buf, len);
}
#define vga_mem_write_string t386_vga_mem_write_string

// pc_new's CPU callbacks, kept as cpui386_new hands them out, for t386_lfb_hooks
#include "i386.h"
static CPU_CB* t386_cb;
static CPUI386* t386_cpui386_new(int gen, char* phys_mem, long phys_mem_size, CPU_CB** cb) {
    CPUI386* cpu = cpui386_new(gen, phys_mem, phys_mem_size, cb);
    t386_cb = *cb;
    return cpu;
}
#define cpui386_new t386_cpui386_new
// The network card's interrupt: 9 as tiny386 has it, or the ini's [pc] ne2000_irq (tab5_386.ino, load_config)
#include "ne2000.h"
int t386_ne2000_irq = 9;
#define isa_ne2000_init(base, irq, pic, f) isa_ne2000_init(base, t386_ne2000_irq, pic, f)
// ... and the linear frame buffer's place, as pc.c's BAR callback maps it (t386_vga_fw.h)
static void t386_set_pci_vga_bar(void* opaque, int bar_num, uint32_t addr, bool enabled);
void t386_tlb_forget(void);   // (t386_fast.h)
#define vga_pci_init(s, bus, opaque, f) vga_pci_init(s, bus, opaque, t386_set_pci_vga_bar)

#define fprintf t386_fprintf
#pragma GCC optimize("O3")
#include "pc.c"

// The linear frame buffer's writes mark what they change (t386_vga_fw.h, t386_vram_mark): pc.c's iomem callbacks,
// wrapped by t386_lfb_hooks() after pc_new
#define T386_LFB_WRITE(f, T, size) \
    static void t386_##f(void* iomem, uword addr, T val) { \
        PC* pc = iomem; \
        if (addr >= pc->pci_vga_ram_addr) t386_vram_mark(addr - pc->pci_vga_ram_addr, size); \
        f(iomem, addr, val); \
    }
T386_LFB_WRITE(iomem_write8, u8, 1)
T386_LFB_WRITE(iomem_write16, u16, 2)
T386_LFB_WRITE(iomem_write32, u32, 4)
static bool t386_iomem_write_string(void* iomem, uword addr, uint8_t* buf, int len) {
    PC* pc = iomem;
    if (addr >= pc->pci_vga_ram_addr && len > 0) t386_vram_mark(addr - pc->pci_vga_ram_addr, len);
    else if (addr - 0xa0000 < 0x10000) t386_vga_written(addr - 0xa0000, len);   // (the window: VBE banks)
    return iomem_write_string(iomem, addr, buf, len);
}
// ... and the VGA window's word and dword reads go to t386_vga_ext.h's fast paths whole (pc.c's read a byte at a time)
#define T386_VGA_READ(f, T, size) \
    static T t386_##f(void* iomem, uword addr) { \
        uint32_t v; \
        if (addr - 0xa0000 < 0x20000 && t386_vga_read_fast(addr - 0xa0000, size, &v)) return v; \
        return f(iomem, addr); \
    }
T386_VGA_READ(iomem_read16, u16, 2)
T386_VGA_READ(iomem_read32, u32, 4)
static void t386_set_pci_vga_bar(void* opaque, int bar_num, uint32_t addr, bool enabled) {
    PC* pc = opaque;
    set_pci_vga_bar(opaque, bar_num, addr, enabled);
    t386_lfb_size = 0;
    t386_lfb_base = addr;
    t386_lfb_ram = pc->vga_mem;
    if (enabled) t386_lfb_size = pc->vga_mem_size;
    t386_tlb_forget();   // (the fast path's TLB may map the old place)
}
#include "t386_accel.h"   // (the 2D engine for Linux's graphics console)
void t386_lfb_hooks(void) {
    t386_cb->iomem_write8 = t386_iomem_write8;
    t386_cb->iomem_write16 = t386_iomem_write16;
    t386_cb->iomem_write32 = t386_iomem_write32;
    t386_cb->iomem_write_string = t386_iomem_write_string;
    t386_cb->iomem_read16 = t386_iomem_read16;
    t386_cb->iomem_read32 = t386_iomem_read32;
    t386_accel_hooks(t386_cb);   // (and the 2D engine's port)
}
