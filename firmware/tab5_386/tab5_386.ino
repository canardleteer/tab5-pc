// Tab5 PC: a PC emulated on the M5Stack Tab5 by tiny386 (https://github.com/hchunhui/tiny386, vendor/tiny386):
// an i386 with VGA, PS/2 keyboard, IDE disks, floppies and SeaBIOS, enough for DOS.
// MIT licence, see LICENSE; the third-party parts and what the firmware is built from: THIRD_PARTY_NOTICES.md.
//
// Core 1 runs the emulated PC flat out (loop()). Core 0 (display_task) polls the keyboard and the serial console,
// and about 30 times a second has tiny386 draw the VGA screen into a 720 x 480 RGB565 buffer, which the PPA scales
// to 4:3 and rotates into the panel's portrait frame buffer: 960 x 720 at most in the middle of the landscape screen,
// the side panels (side_ui.h: power, reset, pause and setup buttons, lights) beside it.
// Every PC video mode DOS uses fits 720 x 480 (text 720 x 400, mode 13h 640 x 400, VGA 640 x 480).
//
// Disks are image files on the SD card (FAT32 or exFAT), set in /tiny386.ini (tools/make_sdcard.py writes one); without one,
// /hda.img, /fda.img and /cda.iso are used if present. The BIOS is built into the firmware.
//
// Serial commands: S (stats) | F (dump the emulated screen, see tools/shot.py) | P (dump the panel's frame buffer,
//                  tools/shot.py --panel) | type <text> (\n Enter, \e Esc,
//                  \b Backspace, \t Tab) | key <hid> [<hid>...] (tap keys by USB HID code, hex, held together;
//                  e0/e1/e2 = Ctrl/Shift/Alt)
//                  | fps <n> (screen refresh rate, 0 = paused) | mem | vol <0-255> | mouse <gain> | wifi on|off | wifi <ssid> <password> | ip | net | raw 0|1 | steps <n> | vgafast 0|1 | peek <addr> [<len>] | panel
//                  | text (the screen's text in a text mode) | stap <x> <y> <ms> (a press on the side panels)
//                  | pd [0|1] (the fast path's pre-decoded blocks; off / on) | reset (the PC's reset button) | poweroff
//                  | shot (a screenshot to /sdcard/screenshots, as the side panel's camera) | clock [YYYY-MM-DD HH:MM:SS (UTC)]
//                  | clock tz <POSIX TZ> (the time zone for local time: screenshots' names, file dates)
//                  | sdbench (the SD card's raw read speed; reads only) | loads (the files loaded at the PC's start and
//                  how long they took)
#include <M5Unified.h>
#include <SD_MMC.h>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>
#include <driver/ppa.h>
#include <esp_cache.h>
#include <soc/ext_mem_defs.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <atomic>
extern "C" {
extern int t386_jit_hot;
extern uint32_t t386_disk_cmds, t386_disk_sectors, t386_disk_hits, t386_disk_retries, t386_disk_prefetched;
extern uint64_t t386_disk_us;
extern uint32_t t386_sync_calls, t386_sync_cycles;
#include "src/tab5_config.h"
#include "src/t386_vga_native.h"
#include "pc.h"
long cpui386_get_cycle(CPUI386* cpu);
void t386_jit_report(char* buf, int len);
void t386_card_bench(char* buf, int len);
extern char t386_load_log[];
long t386_pcmalloc_used(long* len);
void t386_fpu_report(char* buf, int len);
void t386_trace_report(void (*out)(const char*));
void t386_trace_thaw(void);
void t386_jit_hold_on(void);
void t386_jit_hold_off(void);
void t386_trace_bad(void (*out)(const char*));
void t386_jit_inv_report(char* buf, int len);
void t386_jit_code_report(uint32_t lin, void (*out)(const char*));
void t386_jit_sizes_report(void (*out)(const char*));
void t386_jit_ops_report(char* buf, int len);
void t386_fast_ops_report(char* buf, int len);
void t386_ram_written(uint32_t addr, int len);
void t386_cpu_report(CPUI386* cpu, char* buf, int len);
extern void (*t386_shutdown)(void);
extern bool t386_pd_enabled;   // the fast path's pre-decoded blocks (t386_fast_pd.h)
void t386_pd_report(char* buf, int len);
void t386_cpu_regs(CPUI386* cpu, char* buf, int len);
void t386_pic_report(PicState2* s, char* buf, int len);
void t386_port_report(char* buf, int len);
void t386_irq_report(char* buf, int len);
extern int t386_step_max;
uint32_t t386_vga_frame_hash(VGAState* s);
int t386_vga_text(VGAState* s, void (*out)(const char*));
void t386_vga_snapshot(VGAState* s, uint8_t* shadow, size_t shadow_size);
uint32_t t386_mix(uint32_t h, const void* p, size_t n);
extern VGAState* t386_vga;   // the fast video memory paths (src/t386_vga_ext.h)
void t386_vga_fw_update(void);
void t386_lfb_hooks(void);
extern int t386_ne2000_irq;
}

static constexpr const char* FIRMWARE_VERSION = "1.5.0";
static constexpr int EMU_W = 720, EMU_H = 480;     // tiny386's frame buffer (VGA output, centred in it)
static constexpr int SCR_W = 1280, SCR_H = 720;    // landscape
static constexpr int FB_W = 720, FB_H = 1280;      // the panel's native portrait frame buffer
// where the PC's screen goes: 4:3 across the panel's height, the side panels (side_ui.h) in the margins
static constexpr int OUT_W = 960, OUT_H = 720, OUT_X = (SCR_W - OUT_W) / 2;
static int64_t g_frame_us = 33333;                 // screen refresh: ~30 fps (serial: fps <n>, 0 = paused)

static PC* g_pc = nullptr;
static uint16_t* g_emu_fb = nullptr;
static uint8_t* g_vga_shadow = nullptr;            // a copy of the video memory, which each refresh reads
static size_t g_vga_shadow_size = 0;               // (its first 256 KB: the VGA's; VBE modes are drawn natively)
static uint16_t* g_panel_fb = nullptr;
static ppa_client_handle_t g_ppa = nullptr;
static int g_rot = 1;                              // 1 = landscape, 3 = upside down (docked in the keyboard)
// The PC off (side_ui.h: its power button, or the OS powered it off): held, its picture gone, until turned on
static std::atomic<bool> g_pc_off{false};

struct Stats { uint32_t frames, skipped, bands, ppa_errors; uint64_t ppa_us, refresh_us, hash_us; };
static Stats g_stats = {};
static volatile uint64_t g_hpc_cyc = 0, g_hpc_ins = 0;   // core 1's cycles and instructions while the PC runs (serial S)

// ---- input for the PC ---------------------------------------------------------------------------------------
// Keys and mouse movement come from core 0 (keyboard, touch screen, serial), but the PC's keyboard controller and
// interrupt controller belong to core 1, which is running the PC: changed from both sides at once, an interrupt can
// be lost, and with it the keyboard (the program never reads the key waiting for it). So events are queued here
// and handed over by core 1, between steps.
struct InputEvent { int8_t kind; int8_t down_or_buttons; int16_t code_or_dx, dy; };   // kind 0: key, 1: mouse
static QueueHandle_t g_input;   // created in setup()

static void input_put(const InputEvent& e) {
  if (xQueueSend(g_input, &e, 0) != pdTRUE) Serial.println("input: queue full, event dropped");
}

static void input_deliver() {   // core 1, between steps
  InputEvent e;
  while (xQueueReceive(g_input, &e, 0) == pdTRUE) {
    if (e.kind == 0) ps2_put_keycode(g_pc->kbd, e.down_or_buttons, e.code_or_dx);
    else ps2_mouse_event(g_pc->mouse, e.code_or_dx, e.dy, 0, e.down_or_buttons);
  }
}

#include "audio.h"
#include "keyboard.h"
#include "usb_hid.h"
#include "touch.h"

// ---- pausing the PC ----------------------------------------------------------------------------------------
// The PC stops between steps while anyone holds it (the user's pause, an open disk); pc_hold(true) returns once
// it has stopped.
static std::atomic<int> g_hold{0};
// A reset asked for from outside (serial, web, settings, a disk changed) is a cold start: guest RAM cleared, as at
// power-on (an XMS driver that finds a warm start's marker may refuse to load). The guest's own resets (port 92h,
// the keyboard controller: how DOS extenders return to real mode) keep it.
static std::atomic<bool> g_cold_reset{false};
static void pc_cold_reset() { g_cold_reset = true; }
static std::atomic<bool> g_held{false};    // loop() is holding (not inside pc_step)
static bool g_user_pause = false;
// Stopped from the web page (web.h, pc_stop): held for good, its images free to delete or replace; the Tab5 restarts
// to start it again.
static std::atomic<bool> g_pc_stopped{false};

static void pc_hold(bool on) {
  if (!on) { --g_hold; return; }
  ++g_hold;
  while (!g_held) vTaskDelay(1);
}

static void pc_set_paused(bool on) {
  if (on == g_user_pause) return;
  g_user_pause = on;
  pc_hold(on);
}

// Restart the Tab5 more thoroughly than ESP.restart: "a moment of deep sleep", which on the Tab5 tested (chip v1.3)
// ended at once in a watchdog reset of the HP system (rst:0x7 HP_SYS_HP_WDT_RESET, wakeup cause 0), not a wake;
// either resets more than ESP.restart's software reset. After ESP.restart with the PC running from the card until just before,
// the card was now and then missing or the start hung (the card and the Wi-Fi link share the SDMMC host), until the
// Tab5 was switched off: 3 in 30 restarts. This way, 30 of 30 found both.
static void tab5_restart() {
  Serial.flush();
  esp_deep_sleep(100000);
}

#include "net.h"
#include "guest_net.h"
// An option for builds: core 0's cache maintenance on PSRAM (esp_cache_msync, and that of the PPA's and the card's
// drivers) can be kept apart from translated code, which also lives in PSRAM (t386_jit.h, t386_jit_hold_on: core 1
// runs the fast path meanwhile). In long tests, programs that redraw the whole screen all the time occasionally
// stopped with translated code running; these builds made it rarer, not gone. The cause was in the code sync
// (src/t386_i386.c, t386_jit_sync: one write-back pass), fixed; the builds stay as options:
//   DEFS=-DT386_JIT_HOLD=1 tools/build.sh   around those operations: up to ~10% slower
//   DEFS=-DT386_JIT_HOLD=2 tools/build.sh   the whole screen refresh too: up to ~25% slower
#ifndef T386_JIT_HOLD
#define T386_JIT_HOLD 0
#endif
struct JitHold {   // (on core 1 nothing to do: it is between steps)
#if T386_JIT_HOLD
  static inline int depth;   // (core 0's: holds nest)
  const bool on;
  JitHold() : on(xPortGetCoreID() == 0) { if (on && depth++ == 0) t386_jit_hold_on(); }
  ~JitHold() { if (on && --depth == 0) t386_jit_hold_off(); }
#else
  JitHold() {}
#endif
};
static void t386_msync(void* p, size_t n, int flags) {
  JitHold hold;
  esp_cache_msync(p, n, flags);
}

#include "disk_access.h"
#include "clock.h"
#include "web.h"
#include "fd_install.h"
#include "linux_install.h"
#include "cd_drive.h"

static const char* pc_state() {
  return g_pc_stopped ? "stopped" : g_disk.drive >= 0 ? "paused: disk open" : g_user_pause ? "paused" : "running";
}

#include "battery.h"
#include "settings_ui.h"
#include "screenshot.h"
#include "side_ui.h"

// ---- display ------------------------------------------------------------------------------------------------

// Text drawn by M5GFX goes through the CPU cache; write it back (and drop the lines) so a later DMA write by the
// PPA can't be overwritten by stale cache lines.
static void fb_sync() {
  t386_msync(g_panel_fb, (size_t)FB_W * FB_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
}

static void show_status(const char* line1, const char* line2) {
  auto& d = M5.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(middle_center);
  d.setFont(&fonts::Font4);   // (Bodmer's, FreeBSD license; see THIRD_PARTY_NOTICES.md)
  d.setTextColor(0xEF3B);
  d.drawString(line1, SCR_W / 2, SCR_H / 2 - 30);
  d.setTextColor(0x8C94);
  d.drawString(line2, SCR_W / 2, SCR_H / 2 + 25);
  d.display();
  fb_sync();
  Serial.printf("status: %s | %s\n", line1, line2);
}

// The PC's picture on the panel at 4:3, as a monitor showed every mode (text's 720 x 400 dots and 320 x 200
// graphics stretched to the panel's height like 640 x 480: 960 x 720 at most, in the PPA's steps of 1/16). (Up to
// 1.2.1 square pixels too, 1.5 panel pixels per dot, 1080 wide: the margins are the side panels' now.)
static constexpr int ASPECT_W = OUT_W;   // 4:3 at OUT_H
static float q16(float s) { return floorf(s * 16) / 16; }

// Rows y .. y + h, columns x .. x + w of src (pic_w x pic_h) onto the panel, scaled by sx, sy and rotated, their
// top left at (ox, ly) of the landscape screen
static void ppa_block(uint16_t* src, int pic_w, int pic_h, int x, int w, int y, int h, float sx, float sy, int ox,
                      int ly) {
  const int sw = (int)(w * sx), lh = (int)(h * sy);
  ppa_srm_oper_config_t op = {};
  op.in.buffer = src;
  op.in.pic_w = pic_w;
  op.in.pic_h = pic_h;
  op.in.block_w = w;
  op.in.block_offset_x = x;
  op.in.block_offset_y = y;
  op.in.block_h = h;
  op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  op.out.buffer = g_panel_fb;
  op.out.buffer_size = FB_W * FB_H * 2;
  op.out.pic_w = FB_W;
  op.out.pic_h = FB_H;
  op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  if (g_rot == 1) {           // logical (x, y) -> panel (719 - y, x)
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
    op.out.block_offset_x = FB_W - (ly + lh);
    op.out.block_offset_y = ox;
  } else {                    // logical (x, y) -> panel (y, 1279 - x)
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
    op.out.block_offset_x = ly;
    op.out.block_offset_y = FB_H - (ox + sw);
  }
  op.scale_x = sx;
  op.scale_y = sy;
  op.mode = PPA_TRANS_MODE_BLOCKING;
  JitHold hold;
  if (ppa_do_scale_rotate_mirror(g_ppa, &op) != ESP_OK) ++g_stats.ppa_errors;
}

// The PC's part of the panel black (before a picture that doesn't cover it all)
static void clear_pc_area() {
  memset(g_panel_fb + (size_t)FB_W * OUT_X, 0, (size_t)FB_W * OUT_W * 2);
  t386_msync(g_panel_fb + (size_t)FB_W * OUT_X, (size_t)FB_W * OUT_W * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

// The PC's part of the panel at half brightness (paused: its picture dimmed, the side panels and backlight as they
// were). The PPA wrote it: the cache's copy goes first.
static void dim_pc_area() {
  uint16_t* p = g_panel_fb + (size_t)FB_W * OUT_X;
  const size_t n = (size_t)FB_W * OUT_W;
  t386_msync(p, n * 2, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
  uint32_t* q = (uint32_t*)p;   // two pixels at a time: each colour halved (RGB565: 5, 6, 5 bits)
  for (size_t i = 0; i < n / 2; ++i) q[i] = (q[i] >> 1) & 0x7BEF7BEF;
  t386_msync(p, n * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

// tiny386's redraw callback (it always passes the whole buffer): put the bands of BAND_H rows that changed since
// the last time onto the panel. A blinking cursor or a typed character costs one band, not the whole screen.
static constexpr int BAND_H = 16, BANDS = EMU_H / BAND_H;
static uint32_t g_band_hash[BANDS][3];   // its first row, the rows between, its last row
static bool g_bands_valid = false;   // false: copy every band (at the start, or after the panel was drawn on)

static int g_box_w, g_box_h;   // at 4:3: the size of tiny386's picture on the panel (0: none yet)

// tiny386's picture alone (pw x ph, centred in its buffer) scaled to fill the height at 4:3, in bands of BAND_H of
// its rows (whole steps of the scale: 16 rows make a whole number of panel rows)
static void redraw43(int pw, int ph) {
  const int px = (EMU_W - pw) / 2, py = (EMU_H - ph) / 2;
  const float sx = q16((float)ASPECT_W / pw), sy = q16((float)OUT_H / ph);
  const int ox = OUT_X + (OUT_W - (int)(pw * sx)) / 2 & ~7, oy = (OUT_H - (int)(ph * sy)) / 2 & ~7;
  if (pw != g_box_w || ph != g_box_h || !g_bands_valid) {   // another picture: black around it
    clear_pc_area();
    g_box_w = pw, g_box_h = ph;
    g_bands_valid = false;
  }
  // (a band also goes again when the row next to it in the band above or below changed: the PPA's filter mixes that
  // into its first or last panel line, as show_native's groups)
  const int nb = (ph + BAND_H - 1) / BAND_H;
  bool own[BANDS], top[BANDS], bottom[BANDS];
  for (int b = 0; b < nb; ++b) {
    const int y = py + b * BAND_H, n = min(BAND_H, py + ph - y);
    const uint16_t* p = g_emu_fb + y * EMU_W;
    const uint32_t h[3] = {t386_mix(0x811C9DC5u, p, EMU_W * 2),
                           n > 2 ? t386_mix(0x811C9DC5u, p + EMU_W, (n - 2) * EMU_W * 2) : 0,
                           n > 1 ? t386_mix(0x811C9DC5u, p + (n - 1) * EMU_W, EMU_W * 2) : 0};
    top[b] = !g_bands_valid || h[0] != g_band_hash[b][0];
    bottom[b] = !g_bands_valid || h[2] != g_band_hash[b][2];
    own[b] = top[b] || bottom[b] || h[1] != g_band_hash[b][1];
    memcpy(g_band_hash[b], h, sizeof(h));
  }
  int run = -1;
  for (int b = 0; b <= nb; ++b) {
    const bool changed = b < nb && (own[b] || (b > 0 && bottom[b - 1]) || (b + 1 < nb && top[b + 1]));
    if (changed && run < 0) run = b;
    if (!changed && run >= 0) {
      const int y = run * BAND_H, n = min(b * BAND_H, ph) - y;
      ppa_block(g_emu_fb, EMU_W, EMU_H, px, pw, py + y, n, sx, sy, ox, oy + (int)(y * sy));
      g_stats.bands += b - run;
      run = -1;
    }
  }
  g_bands_valid = true;
}

static void redraw(void*, int, int, int, int) {
  const int64_t t0 = esp_timer_get_time();
  t386_msync(g_emu_fb, (size_t)EMU_W * EMU_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  int pw, ph;
  if (!t386_vga_picture(g_pc->vga, &pw, &ph) || pw > EMU_W || ph > EMU_H) pw = EMU_W, ph = EMU_H;   // (all of it)
  redraw43(pw, ph);
  ++g_stats.frames;
  g_stats.ppa_us += esp_timer_get_time() - t0;
}

// The 16- and 256-colour graphics modes: t386_vga_render_native draws the lines that changed at the mode's own size
// into g_native_fb, and the PPA scales just those onto the middle of the PC's part of the panel at 4:3 (mode 13h by 3
// across, 3.5625 down; 12h by 1.5). The sound crackled while the PPA wrote the whole picture every frame (~1.2 MB, 28
// times a second), and again with runs of lines of any size and place; in groups of 16 lines it is clean.
// VBE modes (8 to 32 bits a pixel) too, up to 1024 x 768 when there is the memory for that buffer (else 720 x 480),
// the BIOS offering no larger (t386_vbe_max_w): scaled down to fit when larger than the PC's part of the panel.
static constexpr int NATIVE_W = 720, NATIVE_H = 480, NATIVE_VBE_W = 1024, NATIVE_VBE_H = 768;
static uint16_t* g_native_fb;
static int g_native_w = NATIVE_W, g_native_h = NATIVE_H;   // its size
static uint8_t g_native_dirty[NATIVE_VBE_H];
static T386Native g_native = {};   // what is on the panel (w 0: tiny386's whole picture is)
static bool g_native_on = true;    // serial native 0|1: tiny386's renderer for every mode instead

// The PC's picture as the panel shows it, at its own size (screenshot.h): the native renderer's when it drew the
// panel last (a graphics mode), else tiny386's, centred in its frame buffer (a text mode: the characters' dots)
static bool pc_picture(const uint16_t** px, int* w, int* h, int* stride) {
  if (!g_pc || !g_emu_fb) return false;
  if (g_native.w && g_native_fb) {
    *px = g_native_fb, *w = g_native.w, *h = g_native.h, *stride = g_native.w;
    return true;
  }
  int pw, ph;
  if (!t386_vga_picture(g_pc->vga, &pw, &ph) || pw > EMU_W || ph > EMU_H) pw = EMU_W, ph = EMU_H;
  *px = g_emu_fb + (size_t)(EMU_H - ph) / 2 * EMU_W + (EMU_W - pw) / 2, *w = pw, *h = ph, *stride = EMU_W;
  return true;
}

// The panel brought up to date with what t386_vga_render_native drew (n: its picture)
static void show_native(const T386Native& n) {
  const int64_t t0 = esp_timer_get_time();
  float sx = q16((float)ASPECT_W / n.w), sy = q16((float)OUT_H / n.h);
  const float fit = fminf((float)OUT_W / (n.w * sx), (float)OUT_H / (n.h * sy));   // (VBE: 1024 x 768 at 1.5)
  if (fit < 1) sx = q16(sx * fit), sy = q16(sy * fit);
  const int sw = (int)(n.w * sx), sh = (int)(n.h * sy);
  const int ox = OUT_X + (OUT_W - sw) / 2 & ~7, oy = (OUT_H - sh) / 2 & ~7;   // landscape
  if (n.w != g_native.w || n.h != g_native.h || n.dx != g_native.dx || n.ly != g_native.ly || n.bpp != g_native.bpp) {
    // another shape: black around it (landscape column x is panel row x, or 1279 - x upside down; the picture's
    // lines are panel columns oy .. oy + sh, or 719 - their mirror)
    const int c0 = g_rot == 1 ? FB_W - (oy + sh) : oy, c1 = c0 + sh;
    for (int r = OUT_X; r < OUT_X + OUT_W; ++r) {
      uint16_t* row = g_panel_fb + (size_t)FB_W * r;
      const int x = g_rot == 1 ? r : FB_H - 1 - r;   // (OUT_X .. OUT_X + OUT_W is symmetric: both are in it)
      if (x < ox || x >= ox + sw) {
        memset(row, 0, FB_W * 2);
      } else {
        memset(row, 0, c0 * 2);
        memset(row + c1, 0, (FB_W - c1) * 2);
      }
    }
    t386_msync(g_panel_fb + (size_t)FB_W * OUT_X, (size_t)FB_W * OUT_W * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    if (n.bpp)
      Serial.printf("screen: VBE %d x %d, %d bits a pixel, at %d,%d, %.4g x %.4g\n", n.w, n.h, n.bpp, ox, oy, sx, sy);
    else
      Serial.printf("screen: %d x %d pixels, %d x %d dots each, at %d,%d\n", n.w, n.h, n.dx, n.ly, ox, oy);
  }
  g_native = n;
  t386_msync(g_native_fb, (size_t)n.w * n.h * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  // runs of whole groups of 16 lines (a whole number of panel lines at 4:3), as tiny386's 16-line bands. The PPA's
  // filter mixes the line above a group into its first panel line and the line below into its last, so a group goes
  // again when either of those changed too: else a picture drawn from the bottom up (a startup logo) left its first
  // panel line mixed with the black above, for good.
  const int step = 16;
  g_box_w = 0;
  int run = -1, lines = 0;
  for (int y = 0; y < n.h + step; y += step) {   // (one more group, which only ends a run: a last part group too)
    bool changed = false;
    if (y < n.h)
      for (int i = max(y - 1, 0); i <= y + step && i < n.h; ++i) changed |= g_native_dirty[i] != 0;
    if (changed && run < 0) run = y;
    if (!changed && run >= 0) {
      const int end = min(y, n.h);
      ppa_block(g_native_fb, n.w, n.h, 0, n.w, run, end - run, sx, sy, ox, oy + (int)(run * sy));
      lines += end - run;
      run = -1;
    }
  }
  ++g_stats.frames;
  g_stats.bands += (lines * BANDS + n.h - 1) / n.h;
  g_stats.ppa_us += esp_timer_get_time() - t0;
}

// ---- serial console -----------------------------------------------------------------------------------------

// Typed text: one key event every KEY_STEP_MS, so the PC's 16-byte keyboard buffer never overflows.
static constexpr int64_t KEY_STEP_MS = 25;
static String g_typing;
static int64_t g_type_next = 0;

// The USB HID code (and Shift) that types an ASCII character, US layout; false if none does.
static bool ascii_to_hid(char c, uint8_t& code, bool& shift) {
  static const char lower[] = "abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\#;'`,./";
  static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|~:\"~<>?";
  for (int i = 0; lower[i]; ++i) {
    if (lower[i] == c) { code = 0x04 + i; shift = false; return true; }
    if (upper[i] == c) { code = 0x04 + i; shift = true; return true; }
  }
  return false;
}

static void type_poll() {
  const int64_t now = esp_timer_get_time() / 1000;
  if (g_typing.isEmpty() || now < g_type_next) return;
  uint8_t code;
  bool shift;
  if (ascii_to_hid(g_typing[0], code, shift)) {
    if (shift) pc_key(true, LK_LSHIFT);
    pc_key(true, HID_TO_LINUX[code]);
    pc_key(false, HID_TO_LINUX[code]);
    if (shift) pc_key(false, LK_LSHIFT);
  }
  g_typing.remove(0, 1);
  g_type_next = now + KEY_STEP_MS;
}

static String unescape(const String& s) {
  String out;
  for (size_t i = 0; i < s.length(); ++i) {
    if (s[i] != '\\' || i + 1 == s.length()) { out += s[i]; continue; }
    const char c = s[++i];
    out += c == 'n' ? '\n' : c == 'e' ? '\x1b' : c == 'b' ? '\b' : c == 't' ? '\t' : c;
  }
  return out;
}

static void print_stats() {
  static int64_t t_last = 0;
  static long cyc_last = 0;
  static Stats st_last = {};
  const int64_t now = esp_timer_get_time();
  const long cyc = cpui386_get_cycle(g_pc->cpu);
  const double dcyc = (double)(uint32_t)(cyc - cyc_last);   // (32 bits: it wraps every few minutes)
  const double dt = (now - t_last) / 1e6;
  const uint32_t frames = g_stats.frames - st_last.frames, skipped = g_stats.skipped - st_last.skipped;
  {
    static uint64_t c0 = 0, n0 = 0;
    const uint64_t c = g_hpc_cyc, n = g_hpc_ins;
    static uint32_t last_cmds, last_sectors, last_hits, last_pf;
    static uint64_t last_us;
    Serial.printf("disk: %.0f commands/s, %.0f KB/s, %.0f%% of the time, %.0f reads/s read ahead, %.0f/s prefetched, "
                  "%lu tried again since the start\n",
                  (t386_disk_cmds - last_cmds) / dt, (t386_disk_sectors - last_sectors) / 2.0 / dt,
                  (t386_disk_us - last_us) / 1e4 / dt, (t386_disk_hits - last_hits) / dt,
                  (t386_disk_prefetched - last_pf) / dt, (unsigned long)t386_disk_retries);
    last_cmds = t386_disk_cmds, last_sectors = t386_disk_sectors, last_hits = t386_disk_hits, last_us = t386_disk_us;
    last_pf = t386_disk_prefetched;
    static uint32_t last_sc, last_scyc;
    Serial.printf("code sync: %.0f/s, %.1f%% of core 1\n", (t386_sync_calls - last_sc) / dt,
                  (t386_sync_cycles - last_scyc) / dt / 3.6e6);
    last_sc = t386_sync_calls, last_scyc = t386_sync_cycles;
    Serial.printf("core 1: %.0f M cycles/s, %.0f M host instr/s, IPC %.2f, %.0f host cycles and %.0f host instr per guest instr\n",
                  (c - c0) / dt / 1e6, (n - n0) / dt / 1e6, (double)(n - n0) / (c - c0),
                  (c - c0) / dcyc, (n - n0) / dcyc);
    c0 = c; n0 = n;
  }
  Serial.printf("tab5-pc %s | %.2f M instr/s | %.1f fps drawn, %.1f unchanged (check %.1f ms), redraw %.1f ms "
                "(compare + PPA %.1f ms, %.1f of %d bands%s) | "
                "sound %s%s, peak %u, %u blocks, %u underruns, mix <= %.1f ms | backlight %u | keyboard %s, %u keys | taps %u + %u right | psram free %u | "
                "internal free %u (lowest %u, largest block %u)\n",
                FIRMWARE_VERSION, dcyc / dt / 1e6, frames / dt, skipped / dt,
                frames + skipped ? (g_stats.hash_us - st_last.hash_us) / 1000.0 / (frames + skipped) : 0.0,
                frames ? (g_stats.refresh_us - st_last.refresh_us) / 1000.0 / frames : 0.0,
                frames ? (g_stats.ppa_us - st_last.ppa_us) / 1000.0 / frames : 0.0,
                frames ? (double)(g_stats.bands - st_last.bands) / frames : 0.0, BANDS,
                g_stats.ppa_errors ? ", PPA errors!" : "",
                g_astats.on ? "on" : "off", g_audio_quiet ? " (silenced)" : "", (unsigned)g_astats.peak, g_astats.blocks, g_astats.underruns, g_astats.mix_max_us / 1000.0, (unsigned)g_side_bri,
                g_kbd.present ? "yes" : "no", g_kbd.keys,
                g_tp.taps, g_tp.right_taps,
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  t_last = now;
  cyc_last = cyc;
  st_last = g_stats;
  g_astats.mix_max_us = 0;
  g_astats.peak = 0;
  char batt[128];
  battery_text(batt, sizeof(batt));
  Serial.printf("battery | %s (%d mV) | usb: %u keyboards, %u reports; %u mice, %u reports\n", batt, g_batt.mv,
                g_ustats.keyboards, g_ustats.reports, g_ustats.mice, g_ustats.mouse_reports);
}

// The emulated screen (or the panel's portrait frame buffer) as raw RGB565 (little endian), after a header line
// "FB <w> <h> <bytes>".
static void dump_screen(bool panel) {
  const int w = panel ? FB_W : EMU_W, h = panel ? FB_H : EMU_H;
  const size_t n = (size_t)w * h * 2;
  // the panel's buffer (the PPA writes it, not the CPU) through PSRAM's uncached alias
  const uint8_t* p = panel ? (const uint8_t*)((uintptr_t)g_panel_fb + SOC_NON_CACHEABLE_OFFSET)
                           : (const uint8_t*)g_emu_fb;
  Serial.setTxTimeoutMs(200);   // wait for the host instead of dropping bytes
  const uint32_t sum0 = t386_mix(0x811C9DC5u, p, n);
  Serial.printf("FB %d %d %u\n", w, h, (unsigned)n);
  // 512 bytes at a time, counting only what write() took (the rest again). While the PC is busy
  // (a game with sound) pieces still go missing on the way: tools/shot.py checks the sum and asks again.
  for (size_t off = 0; off < n;) {
    const size_t sent = Serial.write(p + off, min((size_t)512, n - off));
    if (sent == 0) delay(1);
    off += sent;
  }
  const uint32_t sum1 = t386_mix(0x811C9DC5u, p, n);
  Serial.printf("\nSUM %08x\n", (unsigned)sum1);   // tools/shot.py checks what it got
  if (sum0 != sum1) Serial.printf("dump: the picture changed while it was sent (%08x before)\n", (unsigned)sum0);
  Serial.setTxTimeoutMs(0);
}

// the guest's own resets (port 92h, the keyboard controller: loop() sees reset_request), for serial "cpu"
static volatile uint32_t g_reset_count, g_reset_ms;
static char g_reset_regs[400];

static void handle_serial() {
  static String line;
  while (Serial.available()) {
    const char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (line.length() < 512) line += c;
      continue;
    }
    if (line.isEmpty()) continue;
    if (line == "S") {
      print_stats();
    } else if (line == "F" || line == "P") {
      dump_screen(line == "P");
    } else if (line == "text") {   // the screen's text, if it shows text ("TEXT" lines)
      if (!t386_vga_text(g_pc->vga, [](const char* l) { Serial.printf("TEXT %s\n", l); })) Serial.println("TEXT (graphics)");
    } else if (line == "PS") {   // a quick checksum of what the panel shows, every 4th word (has the screen settled?)
      const uint32_t* p = (const uint32_t*)((uintptr_t)g_panel_fb + SOC_NON_CACHEABLE_OFFSET);
      uint32_t h = 0x811C9DC5u;
      for (size_t i = 0; i < (size_t)FB_W * FB_H / 2; i += 4) h = (h ^ p[i]) * 16777619u;
      Serial.printf("PSUM %08x\n", (unsigned)h);
    } else if (line.startsWith("type ")) {
      g_typing += unescape(line.substring(5));
    } else if (line.startsWith("key ")) {   // key 1d 3a: press HID 0x1d and 0x3a together, then release
      uint8_t codes[8];
      int n = 0;
      for (char* t = strtok((char*)line.c_str() + 4, " "); t && n < 8; t = strtok(nullptr, " "))
        codes[n++] = (uint8_t)strtol(t, nullptr, 16);
      // HID usage -> Linux key code; e0/e1/e2 are left Ctrl/Shift/Alt
      auto lk = [](uint8_t c) { return c == 0xE0 ? LK_LCTRL : c == 0xE1 ? LK_LSHIFT : c == 0xE2 ? LK_LALT
                                       : c < sizeof(HID_TO_LINUX) ? HID_TO_LINUX[c] : 0; };
      for (int i = 0; i < n; ++i) pc_key(true, lk(codes[i]));
      delay(50);
      for (int i = n - 1; i >= 0; --i) pc_key(false, lk(codes[i]));
    } else if (line == "native 0" || line == "native 1") {
      g_native_on = line == "native 1";
      Serial.printf("native renderer %s\n", g_native_on ? "on" : "off");
    } else if (line.startsWith("fps ")) {
      const int fps = line.substring(4).toInt();
      g_frame_us = fps > 0 ? 1000000 / fps : 0;
      Serial.printf("screen refresh %d fps\n", fps);
    } else if (line == "fast ops") {   // the instructions the fast path hands to tiny386, the most first
      char buf[600];
      t386_fast_ops_report(buf, sizeof(buf));
      Serial.print(buf);
    } else if (line == "jit inv" || line == "jit ops") {   // the last invalidated translations; the opcodes run in place
      static char* buf = (char*)heap_caps_malloc(2600, MALLOC_CAP_SPIRAM);   // (internal RAM: WiFi's)
      if (!buf) return;
      if (line == "jit inv") t386_jit_inv_report(buf, 2600);
      else t386_jit_ops_report(buf, 2600);
      for (char* p = buf; *p; ) {   // a line at a time: the USB serial drops what doesn't fit its buffer
        char* e = strchr(p, '\n');
        if (e) *e = 0;
        Serial.println(p);
        delay(3);
        if (!e) break;
        p = e + 1;
      }
#ifdef T386_TRACE
    } else if (line == "trace" || line == "trace thaw") {   // the translator's last steps (t386_jit.h)
      if (line == "trace thaw") {
        t386_trace_thaw();
        return;
      }
      t386_trace_report([](const char* l) { Serial.println(l); delay(2); });
      t386_trace_bad([](const char* l) { Serial.println(l); delay(2); });
#endif
#ifdef T386_FPU_STATS
    } else if (line == "fpu") {   // the x87 instructions translated code ran, the helpers' quick ways (t386_jit.h)
      static char* buf = (char*)heap_caps_malloc(2000, MALLOC_CAP_SPIRAM);
      if (!buf) return;
      t386_fpu_report(buf, 2000);
      for (char* p = buf; *p; ) {
        char* e = strchr(p, '\n');
        if (e) *e = 0;
        Serial.println(p);
        delay(3);
        if (!e) break;
        p = e + 1;
      }
#endif
    } else if (line == "loads") {   // the files loaded at the PC's start and how long they took (src/host.c)
      for (char* p = t386_load_log; *p; ) {
        char* e = strstr(p, "; ");
        if (e) *e = 0;
        Serial.println(p);
        if (e) *e = ';';
        delay(3);
        if (!e) break;
        p = e + 2;
      }
      if (!t386_load_log[0]) Serial.println("loads: none from the card");
      long pc_len, pc_used = t386_pcmalloc_used(&pc_len);
      Serial.printf("device state (pcmalloc, 640K-768K): %ld of %ld bytes used\n", pc_used, pc_len);
      Serial.printf("raw access: the last image mapped in %u ms\n", (unsigned)g_runs_ms);
    } else if (line == "sdbench") {   // the card's raw read speed (src/t386_ide.c; reads only)
      static char buf[900];
      t386_card_bench(buf, sizeof(buf));
      for (char* p = buf; *p; ) {
        char* e = strstr(p, "; ");
        if (e) *e = 0;
        Serial.println(p);
        delay(3);
        if (!e) break;
        p = e + 2;
      }
    } else if (line == "jit") {
      static char buf[900];
      t386_jit_report(buf, sizeof(buf));
      for (char* p = buf; *p; ) {   // in pieces at "; ": the USB serial drops what doesn't fit its buffer
        char* e = strstr(p, "; ");
        if (e) *e = 0;
        Serial.println(p);
        delay(3);
        if (!e) break;
        p = e + 2;
      }
#ifdef T386_JIT_SIZES
    } else if (line == "jit sizes") {   // the words of the translations made since the last time, by kind (t386_jit.h)
      t386_jit_sizes_report([](const char* l) { Serial.println(l); delay(1); });
#endif
    } else if (line.startsWith("jit code ")) {   // a block's x86 bytes and translated code, as hex (t386_jit.h)
      t386_jit_code_report(strtoul(line.c_str() + 9, nullptr, 16), [](const char* l) { Serial.println(l); delay(1); });
    } else if (line.startsWith("jit hot ")) {   // translate code once reached this many times (t386_jit.h)
      t386_jit_hot = constrain(line.substring(8).toInt(), 0, 255);
      Serial.printf("jit hot %d\n", t386_jit_hot);
    } else if (line.startsWith("jit xlink ")) {   // the translator's links across pages on or off (t386_jit.h)
      extern int t386_jit_xlink;
      t386_jit_xlink = line.substring(10).toInt();
      Serial.printf("xlink %d\n", t386_jit_xlink);
    } else if (line == "pd") {   // the fast path's pre-decoded blocks (t386_fast_pd.h)
      static char buf[1200];
      t386_pd_report(buf, sizeof(buf));
      Serial.print(buf);
    } else if (line == "pd 0" || line == "pd 1") {   // pre-decoding off or on
      t386_pd_enabled = line == "pd 1";
      Serial.printf("pd %s\n", t386_pd_enabled ? "on" : "off");
    } else if (line == "jit 0" || line == "jit 1") {   // the translator on or off (t386_jit.h)
      extern bool t386_jit_enabled;
      t386_jit_enabled = line == "jit 1";
      Serial.printf("jit %s\n", t386_jit_enabled ? "on" : "off");
    } else if (line == "cpu") {   // where the guest program is (sampled while it runs), and the interrupts
      char buf[400];
      t386_cpu_regs(g_pc->cpu, buf, sizeof(buf));
      Serial.println(buf);
      t386_pic_report(g_pc->pic, buf, sizeof(buf));
      Serial.println(buf);
      t386_port_report(buf, sizeof(buf));
      Serial.println(buf);
      t386_irq_report(buf, sizeof(buf));
      Serial.println(buf);
      Serial.printf("the Tab5's last restart: reason %d (4: a panic)\n", (int)esp_reset_reason());
      if (g_reset_count)
        Serial.printf("guest resets: %u, the last %lu s ago at: %s\n", (unsigned)g_reset_count,
                      (unsigned long)((millis() - g_reset_ms) / 1000), g_reset_regs);
    } else if (line.startsWith("peek ")) {   // peek <hex address> [<hex length>]: guest RAM, as hex ("PEEK" lines)
      char* end;
      uint32_t a = strtoul(line.c_str() + 5, &end, 16), n = strtoul(end, nullptr, 16);
      if (!n) n = 0x100;
      const uint32_t size = g_pc->phys_mem_size;
      for (uint32_t o = 0; o < n && a + o < size; o += 32) {
        char out[120];
        int k = snprintf(out, sizeof(out), "PEEK %08lx", (unsigned long)(a + o));
        for (uint32_t i = 0; i < 32 && o + i < n && a + o + i < size; ++i)
          k += snprintf(out + k, sizeof(out) - k, " %02x", g_pc->phys_mem[a + o + i]);
        Serial.println(out);
      }
    } else if (line == "mem") {   // where the hot data lives (internal RAM is faster than PSRAM)
      char buf[160];
      t386_cpu_report(g_pc->cpu, buf, sizeof(buf));
      Serial.printf("DMA-capable internal RAM: %u free, largest block %u (Wi-Fi, the card, TLS's AES need it)\n",
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
      Serial.printf("%s | PC struct %s | guest RAM %p\n", buf, esp_ptr_external_ram(g_pc) ? "PSRAM" : "internal",
                    g_pc->phys_mem);
    } else if (line.startsWith("stap ")) {   // stap <x> <y> <ms>: a press beside the PC's picture (side_ui.h), held ms
      int x = 0, y = 0, ms = 100;
      sscanf(line.c_str() + 5, "%d %d %d", &x, &y, &ms);
      g_stap_x = x;
      g_stap_y = y;
      side_touch(x, y, 1, esp_timer_get_time() / 1000);
      g_stap_until = esp_timer_get_time() / 1000 + ms;
    } else if (line.startsWith("ptap ")) {   // ptap <x> <y>: a tap on the settings panel (1280 x 720, landscape)
      int x = 0, y = 0;
      sscanf(line.c_str() + 5, "%d %d", &x, &y);
      g_fake_x = x;
      g_fake_y = y;
      g_fake_reads = 4;
    } else if (line.startsWith("mev ")) {   // mev <dx> <dy> <buttons>: a PS/2 mouse event (dy up; buttons 1 left 2 right)
      int dx = 0, dy = 0, b = 0;
      sscanf(line.c_str() + 4, "%d %d %d", &dx, &dy, &b);
      input_put({1, (int8_t)b, (int16_t)dx, (int16_t)dy});
    } else if (line.startsWith("mouse ")) {   // touchpad speed: PS/2 counts per pixel of finger movement
      g_mouse_gain = constrain(line.substring(6).toFloat(), 0.1f, 8.0f);
      Serial.printf("mouse gain %.2f\n", g_mouse_gain);
    } else if (line == "vgafast 0" || line == "vgafast 1") {   // the fast video memory paths (src/t386_vga_ext.h)
      t386_vga = line == "vgafast 1" ? g_pc->vga : nullptr;
      t386_vga_fw_update();
      Serial.println(line);
    } else if (line.startsWith("steps ")) {   // longest run of instructions between device polls (src/t386_pc.c)
      t386_step_max = constrain(line.substring(6).toInt(), 512, 8192);
      Serial.printf("steps %d\n", t386_step_max);
    } else if (line.startsWith("vol ")) {
      set_volume(line.substring(4).toInt());
    } else if (line == "wifi on" || line == "wifi off") {
      wifi_enable(line == "wifi on");
    } else if (line.startsWith("wifi ")) {   // wifi <ssid> <password> (switches WiFi on)
      const String rest = line.substring(5);
      const int sp = rest.indexOf(' ');
      wifi_start(sp < 0 ? rest : rest.substring(0, sp), sp < 0 ? "" : rest.substring(sp + 1));
    } else if (line == "net") {   // the PC's network (guest_net.h)
      guest_net_report();
    } else if (line == "ip") {
      Serial.printf("wifi %s, %s, http://%s/\n", g_wifi == WIFI_UP ? "up" : g_wifi == WIFI_DISABLED ? "off" : "down", g_ssid.c_str(),
                    WiFi.localIP().toString().c_str());
    } else if (line.startsWith("raw ")) {   // raw 0|1: disk images as raw card sectors (from the next start)
      g_prefs.putBool("raw", line.substring(4).toInt() != 0);
      Serial.println("raw access: from the next start");
    } else if (line == "shot") {   // a screenshot, as the side panel's camera (screenshot.h)
      shot_start(esp_timer_get_time() / 1000);
    } else if (line == "clock" || line.startsWith("clock ")) {   // clock [YYYY-MM-DD HH:MM:SS]: the Tab5's clock
      clock_command(line.length() > 6 ? line.substring(6) : String());
    } else if (line.startsWith("i2c ")) {   // i2c <addr> <reg> [<value>]: read or write a register on the internal I2C
      unsigned a = 0, r = 0, v = 0;   // bus (codec 10, IO expanders 43/44, touch...), hex
      const int n = sscanf(line.c_str() + 4, "%x %x %x", &a, &r, &v);
      if (n >= 2) {
        xSemaphoreTake(g_i2c_lock, portMAX_DELAY);
        const bool ok = n == 3 ? M5.In_I2C.writeRegister8(a, r, v, 400000) : true;
        const uint8_t now = M5.In_I2C.readRegister8(a, r, 400000);
        xSemaphoreGive(g_i2c_lock);
        Serial.printf("i2c %02x %02x: %s%02x\n", a, r, ok ? "" : "(write failed) ", now);
      }
    } else if (line == "codec") {   // the speaker codec's registers (audio.h)
      audio_codec_report();
    } else if (line == "spk restart") {   // the speaker codec set up again, its sound running on (audio.h)
      audio_spk_restart();
    } else if (line == "beep") {   // a speaker check without the PC: the amplifier on, an 880 Hz tone on another channel
      xSemaphoreTake(g_i2c_lock, portMAX_DELAY);
      Serial.printf("speaker started %d, volume %u (M5 %u)\n", g_spk_started, g_volume, M5.Speaker.getVolume());
      if (!g_spk_started) g_spk_started = M5.Speaker.begin();
      M5.Speaker.setVolume(g_volume);   // (the volume the PC's sound has: the audio task sets it only when it changes)
      audio_amp(true);
      uint8_t r5 = 0, r3 = 0;
      M5.In_I2C.readRegister(0x43, 0x05, &r5, 1, 400000);
      M5.In_I2C.readRegister(0x43, 0x03, &r3, 1, 400000);
      M5.Speaker.tone(880, 1500, 3);
      xSemaphoreGive(g_i2c_lock);
      Serial.printf("expander 0x43: out (reg 5) %02x, direction (reg 3) %02x; tone 880 Hz 1.5 s\n", r5, r3);
    } else if (line == "panel") {
      panel_toggle();
    } else if (line.startsWith("dns ")) {   // dns <host>: look a name up (Wi-Fi up), and how long it took
      const int64_t t0 = esp_timer_get_time();
      IPAddress ip;
      const int ok = WiFi.hostByName(line.substring(4).c_str(), ip);
      Serial.printf("dns: %s -> %s (%s, %lld ms; DNS server %s)\n", line.substring(4).c_str(), ip.toString().c_str(),
                    ok == 1 ? "ok" : "failed", (long long)((esp_timer_get_time() - t0) / 1000), WiFi.dnsIP().toString().c_str());
    } else if (line == "machines") {   // the card's machines (linux_install.h); * the one running
      machines_scan();
      for (int i = 0; i < g_machine_count; ++i)
        Serial.printf("%c %s  (%s)\n", machine_path() == "/sdcard/" + String(g_machines[i].file) ? '*' : ' ',
                      g_machines[i].file, g_machines[i].name);
    } else if (line.startsWith("machine ")) {   // machine <file>: start that one (the Tab5 restarts)
      machine_choose(line.c_str() + 8);
      restart_tab5();
    } else if (line.startsWith("install linux ") || line.startsWith("install dos ")) {   // install linux|dos <name>:
      const bool dos = line.startsWith("install dos ");                                 // as the panel's buttons
      const String name = line.substring(dos ? 12 : 14);
      if (!machine_name_ok(name.c_str())) Serial.println("install: a name of letters, digits, - and _");
      else if (machine_name_used(name.c_str()) && (dos || !lx_installed(name.c_str()))) Serial.println("install: that name is in use");
      else install_request(dos ? 'd' : 'l', name.c_str());   // (the Tab5 restarts into the install)
    } else if (line.startsWith("install iso ")) {   // install iso <name> [<file>.iso]: as the panel's Install ISO
      String name = line.substring(12), iso;
      if (name.indexOf(' ') > 0) iso = name.substring(name.indexOf(' ') + 1), name = name.substring(0, name.indexOf(' '));
      fd_find_isos();
      if (iso.isEmpty() && g_fd_iso_count) iso = g_fd_isos[0];
      bool found = false;
      for (int i = 0; i < g_fd_iso_count; ++i) found |= g_fd_isos[i].equalsIgnoreCase(iso);
      if (!machine_name_ok(name.c_str())) Serial.println("install: a name of letters, digits, - and _");
      else if (machine_name_used(name.c_str())) Serial.println("install: that name is in use");
      else if (!found) Serial.println("install: no such .iso at the card's root (1 MB or more)");
      else install_request('i', (name + ":" + iso).c_str());
    } else if (line == "cd" || line.startsWith("cd ")) {   // cd [<file>.iso | -]: the disc in the drive, or change it
      String a = line.length() > 3 ? line.substring(3) : String();
      a.trim();
      if (a.isEmpty()) Serial.printf("cd: %s\n", g_cd_drive < 0 ? "no drive" : cd_disc()[0] ? cd_disc() : "no disc");
      else cd_change(a == "-" ? "" : a.startsWith("/") ? a.c_str() : ("/sdcard/" + a).c_str());
    } else if (line == "linux") {
      Serial.printf("linux: %s (%d%%)\n", g_lx_msg, (int)g_lx_pct);
    } else if (line == "reset") {
      pc_cold_reset();
      Serial.println("reset");
    } else if (line == "poweroff") {   // the PC stops (between instructions), the screen goes dark, the power off
      pc_hold(true);
      Serial.println("power off");
      Serial.flush();
      M5.Display.setBrightness(0);
      xSemaphoreTake(g_i2c_lock, portMAX_DELAY);
      M5.Power.powerOff();
      xSemaphoreGive(g_i2c_lock);
      delay(2000);   // still here (on USB power, the power switch may not cut it): sleep for good instead
      esp_deep_sleep_start();
    } else {
      Serial.println("? S | F | P | type <text> | key <hid>... | reset");
    }
    line = "";
  }
}

// Core 0: keyboard, serial, and the screen (the vertical retrace moves on as the PC reads it: src/t386_pc.c). Every
// g_frame_us the VGA state is fingerprinted, and only if it changed is the screen redrawn (tiny386 renders it from a
// copy of the video memory, the PPA puts it on the panel: ~30-80 ms, competing with the PC for PSRAM).
static void display_task(void*) {
  int64_t last_refresh = 0;
  uint32_t last_hash = 0;
  bool stopped_shown = false, off_shown = false, paused_shown = false;
  // the PC's picture onto the panel where it changed (native modes by their changed lines, others by bands)
  auto refresh = [&](int64_t now) {
#if T386_JIT_HOLD == 2
    JitHold whole;   // (the whole refresh, its PSRAM writes too)
#endif
    T386Native n;
    if (g_native_on && g_native_fb &&
        t386_vga_render_native(g_pc->vga, g_native_fb, g_native_dirty, g_native_w, g_native_h, g_native.w == 0, &n)) {
      // (the renderer fingerprints the lines itself: no whole-frame fingerprint first; its time counts as the
      // check's, and the conversion of the lines that changed with it)
      const int64_t t1 = esp_timer_get_time();
      g_stats.hash_us += t1 - now;
      if (n.changed) {
        show_native(n);
        g_stats.refresh_us += esp_timer_get_time() - t1;
      } else {
        ++g_stats.skipped;
      }
      last_hash = 0;
    } else {
      const uint32_t hash = t386_vga_frame_hash(g_pc->vga);
      const int64_t t1 = esp_timer_get_time();
      g_stats.hash_us += t1 - now;
      if (hash != last_hash) {   // only when the picture changed
        last_hash = hash;
        if (g_native.w) {   // from a native picture to tiny386's: all of it again (it covers the margins)
          g_native.w = 0;
          g_bands_valid = false;
        }
        t386_vga_snapshot(g_pc->vga, g_vga_shadow, g_vga_shadow_size);
        vga_refresh(g_pc->vga, redraw, nullptr, g_pc->full_update != 0);
        if (g_pc->full_update == 2) g_pc->full_update = 0;
        g_stats.refresh_us += esp_timer_get_time() - t1;
      } else {
        ++g_stats.skipped;
      }
    }
  };
  if (g_setup_needed) {   // nothing on the card yet: Wi-Fi on, and the panel's Install DOS / Install Linux
    wifi_enable(true);
    panel_open();
  } else if (g_prefs.getBool("wifiback", false)) {   // restarted with WiFi on (web.h, restart_tab5): on again
    g_prefs.remove("wifiback");
    wifi_enable(true);
  }
  for (;;) {
    kbd_poll();
    usb_kbd_poll();
    battery_poll();
    touch_poll();
    handle_serial();
    wifi_poll();
    clock_poll();
    disk_poll();
    type_poll();
    const int64_t now = esp_timer_get_time();
    if (g_panel_open) {   // the settings panel instead of the PC's screen
      side_state_poll();   // (pause or resume in the panel: the sound follows at once)
      ui_poll();
      vTaskDelay(1);
      continue;
    }
    if (g_redraw_pc) {    // back from the panel: everything again, and black margins beside the PC's screen
      g_redraw_pc = false;
      // the margins (landscape x < OUT_X and x >= OUT_X + OUT_W) are whole rows of the portrait frame buffer
      memset(g_panel_fb, 0, (size_t)FB_W * OUT_X * 2);
      memset(g_panel_fb + (size_t)FB_W * (OUT_X + OUT_W), 0, (size_t)FB_W * (FB_H - OUT_X - OUT_W) * 2);
      fb_sync();
      last_hash = 0;
      g_bands_valid = false;
      g_native.w = 0;
      g_box_w = 0;
      last_refresh = 0;
      stopped_shown = false;
      off_shown = false;
      paused_shown = false;
      side_invalidate();
    }
    if (g_pc_stopped) {   // (web.h, pc_stop) the PC's screen no longer changes: say why
      if (!stopped_shown) {
        show_status("The PC is stopped", "Start it from the web page, or restart the Tab5");
        side_invalidate();
      }
      stopped_shown = true;
      side_poll(now / 1000);
      vTaskDelay(20);
      continue;
    }
    if (g_pc_off) {   // (side_ui.h) its picture gone, a word where it was
      if (!off_shown) {
        clear_pc_area();
        auto& d = M5.Display;
        d.setTextDatum(middle_center);
        d.setFont(&fonts::Font4);
        d.setTextColor(0x632C);
        d.drawString("The PC is off", SCR_W / 2, SCR_H / 2 - 18);
        d.setFont(&fonts::Font2);
        d.setTextColor(0x4A49);
        d.drawString(g_side_on ? "Tap POWER to turn it on" : "Tap to turn it on", SCR_W / 2, SCR_H / 2 + 20);
        fb_sync();
        g_box_w = 0;
        last_hash = 0;
        g_bands_valid = false;
        g_native.w = 0;
        off_shown = true;
      }
      side_poll(now / 1000);
      vTaskDelay(10);
      continue;
    }
    if (off_shown) {   // on again: the picture in full
      off_shown = false;
      clear_pc_area();
    }
    side_poll(now / 1000);
    // Paused (PAUSE, the settings panel's Pause, the web page): the picture brought up to date once, then dimmed and
    // left alone (a text mode's cursor stops blinking too); in full again when the PC goes on
    if (g_user_pause) {
      if (!paused_shown) {
        refresh(now);
        dim_pc_area();
        paused_shown = true;
      }
      vTaskDelay(10);
      continue;
    }
    if (paused_shown) {
      paused_shown = false;
      last_hash = 0;
      g_bands_valid = false;
      g_native.w = 0;
      g_box_w = 0;
      last_refresh = 0;
    }
    if (g_frame_us && now - last_refresh >= g_frame_us) {
      last_refresh = now;
      refresh(now);
    }
    vTaskDelay(1);
  }
}

// ---- machine setup ------------------------------------------------------------------------------------------

static bool sd_mount() {
  // SD_MMC slot 0 on the P4's fixed pins; the board variant powers the card (LDO channel 4)
  if (SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_HIGHSPEED)) return true;
  if (SD_MMC.begin("/sdcard", false, false, SDMMC_FREQ_DEFAULT)) return true;
  // After a software reset (a panic) the card can be missing until a fuller reset: tab5_restart's way finds it.
  // Once (the reset reason is a watchdog's then): a Tab5 without a card goes on without one.
  const esp_reset_reason_t r = esp_reset_reason();
  if (r == ESP_RST_SW || r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT) {
    Serial.println("SD card: not found after a restart: restarting more thoroughly");
    tab5_restart();
  }
  return false;
}

static bool file_exists(const char* path) {
  FILE* f = fopen(path, "rb");
  if (f) fclose(f);
  return f != nullptr;
}

// [pc] ne2000_irq = <n>: the network card's interrupt, 9 unless set (tiny386's). Some NE2000 drivers don't work on 9
// (the second interrupt controller's line that stands in for 2): 3, say, for them.
static int ne2000_ini_handler(void*, const char* section, const char* name, const char* value) {
  if (!strcmp(section, "pc") && !strcmp(name, "ne2000_irq")) {
    const int irq = atoi(value);
    if (irq == 3 || irq == 5 || irq == 7 || irq == 9 || irq == 10 || irq == 11 || irq == 15) t386_ne2000_irq = irq;
    else Serial.printf("config: ne2000_irq = %s: not a free interrupt (3, 5, 7, 9, 10, 11, 15), 9 kept\n", value);
  }
  return 1;
}

// An install asked for in the settings panel (Install DOS / Install Linux / Install ISO, linux_install.h): the Tab5
// restarted for it and does it instead of starting a PC, with all the memory free and nothing else using the card or
// Wi-Fi, then restarts into the new machine (DOS: FreeDOS's installer; Linux: its first start; ISO: the CD). If it
// fails it says why and, after a tap on the screen or a minute, restarts into the machine there was. Wi-Fi (not for
// an ISO: nothing to download): the saved network, joined here directly (no web server, no PC network: they want a
// PC).
static void install_mode_run() {
  const String req = g_prefs.getString("install", "");
  if (req.length() < 3 || req[1] != ':') return;
  g_prefs.remove("install");   // (once: a crash doesn't start it again)
  const char kind = req[0];
  String name = req.substring(2), iso;
  if (kind == 'i' && name.indexOf(':') > 0) iso = name.substring(name.indexOf(':') + 1), name = name.substring(0, name.indexOf(':'));
  const String title = (kind == 'd' ? String("Installing DOS") : kind == 'l' ? String("Installing Linux") : "Installing from " + iso) +
                       " as " + name;
  g_held = true;   // (no PC to hold: pc_hold returns at once)
  String failed;
  if (kind != 'i') {   // (from a CD image: nothing to download)
    show_status(title.c_str(), "Connecting to Wi-Fi ...");
    wifi_load();
  }
  if (kind == 'i') {
    if (iso.isEmpty()) failed = "No CD image given";
  } else if (g_ssid.isEmpty()) {
    failed = "No Wi-Fi network is set up";
  } else {
    heap_caps_malloc_extmem_enable(512);   // (as wifi_enable: lwIP's packets in PSRAM)
    WiFi.mode(WIFI_STA);
    WiFi.begin(g_ssid.c_str(), g_pass.c_str());
    for (int i = 0; i < 300 && WiFi.status() != WL_CONNECTED; ++i) delay(100);
    if (WiFi.status() != WL_CONNECTED) failed = "Could not connect to " + g_ssid;
  }
  if (failed.isEmpty()) {
    if (kind != 'i') {
      WiFi.setSleep(false);   // (modem power save: ~50 KB/s)
      g_wifi = WIFI_UP;
    }
    Serial.printf("install: %s%s (DMA-capable internal RAM %u free, PSRAM %u)\n", req.c_str(), kind != 'i' ? ", Wi-Fi up" : "",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (kind == 'i') fd_install_start(name.c_str(), iso.c_str());
    else if (kind == 'd') fd_install_start(name.c_str());
    else lx_install_start(name.c_str());
    const bool fd = kind != 'l';   // (fd_install.h's, DOS or the CD)
    String shown;
    for (;;) {
      const FdState st = fd ? g_fd_state : g_lx_state;
      const char* msg = fd ? g_fd_msg : g_lx_msg;
      if (shown != msg) show_status(title.c_str(), (shown = msg).c_str());
      if (st != FD_RUNNING) break;
      delay(250);
    }
    if ((fd ? g_fd_state : g_lx_state) == FD_IDLE) {   // done: the new machine from now on (DOS: its installer)
      machine_choose(("tiny386-" + name + ".ini").c_str());
      show_status(title.c_str(), "Done. Restarting into it ...");
      delay(1500);
      tab5_restart();
    }
    failed = fd ? g_fd_msg : g_lx_msg;   // (FD_FAILED)
  }
  show_status(("The install failed: " + failed).c_str(), "Tap the screen to go back (or wait a minute)");
  for (int i = 0; i < 600; ++i) {
    M5.update();
    if (M5.Touch.getCount()) break;
    delay(100);
  }
  tab5_restart();
}

static void load_config(PCConfig& conf, bool have_sd) {
  memset(&conf, 0, sizeof(conf));
  conf.mem_size = 16 * 1024 * 1024;
  conf.vga_mem_size = 2 * 1024 * 1024;
  conf.cpu_gen = 4;
  conf.fpu = 0;
  conf.fill_cmos = 1;
  conf.vga_force_8dm = 1;   // text 8 dots wide (640 x 400): the 9th column of the VGA's cells, scaled 4/3 to the
                            // panel, striped shaded backgrounds; an ini's vga_force_8dm = 0 brings it back
  const String ini = machine_path();   // (/sdcard/tiny386.ini, or the machine chosen in the panel: linux_install.h)
  // an installer running in it (fd_install.h), and now a system on its C.IMG: the install is over
  fd_use(ini);
  if (have_sd && ini_parse(FD_INI, fd_ini_handler, &g_install_mode) == 0 && g_install_mode) fd_install_finish();
  if (have_sd && ini_parse(ini.c_str(), parse_conf_ini, &conf) == 0) {
    Serial.printf("config: %s\n", ini.c_str());
    ini_parse(ini.c_str(), ne2000_ini_handler, nullptr);
    if (t386_ne2000_irq != 9) Serial.printf("config: the network card on IRQ %d\n", t386_ne2000_irq);
  } else if (have_sd) {
    if (file_exists("/sdcard/hda.img")) conf.disks[0] = "/sdcard/hda.img";
    if (file_exists("/sdcard/cda.iso")) conf.disks[2] = "/sdcard/cda.iso", conf.iscd[2] = 1;
    if (file_exists("/sdcard/fda.img")) conf.fdd[0] = "/sdcard/fda.img";
    Serial.println("config: defaults (no /sdcard/tiny386.ini)");
  }
  conf.width = EMU_W;   // the screen layout is fixed here, whatever the ini says
  conf.height = EMU_H;
  // a disk that can't be opened would stop the emulator (assert); leave it out and say so (a CD-ROM drive: empty)
  for (int i = 0; i < 4; ++i)
    if (conf.disks[i] && conf.disks[i][0] && !file_exists(conf.disks[i])) {
      Serial.printf("disk %d: %s not found, %s\n", i, conf.disks[i], conf.iscd[i] ? "the drive empty" : "left out");
      conf.disks[i] = conf.iscd[i] ? "" : nullptr;
    }
  for (int i = 0; i < 2; ++i)
    if (conf.fdd[i] && conf.fdd[i][0] && !file_exists(conf.fdd[i])) {
      Serial.printf("floppy %d: %s not found, left out\n", i, conf.fdd[i]);
      conf.fdd[i] = nullptr;
    }
  // an empty card: the settings panel opens, to install a machine (CD images don't count)
  g_setup_needed = have_sd && !file_exists("/sdcard/tiny386.ini") && !conf.disks[0] && !conf.disks[1] &&
                   !conf.fdd[0] && !conf.fdd[1];
  cd_setup(conf);   // (cd_drive.h: a CD-ROM drive, empty if none was given)
  Serial.printf("pc: %ld KB RAM, %ld KB video, %d86%s | hda %s | hdb %s | hdc %s | hdd %s | fda %s | fdb %s\n",
                conf.mem_size / 1024, conf.vga_mem_size / 1024, conf.cpu_gen, conf.fpu ? " + FPU" : "",
                conf.disks[0] ?: "-", conf.disks[1] ?: "-", conf.disks[2] ?: "-", conf.disks[3] ?: "-",
                conf.fdd[0] ?: "-", conf.fdd[1] ?: "-");
}

// M5GFX makes the panel with two frame buffers and uses the first alone. The panel shows the first unless
// esp_lcd_panel_draw_bitmap is given a pointer into the second, which nothing here does: the second (1.8 MB of PSRAM)
// is given back to the heap, for the translator's code. (The driver keeps its address; the panel is never deleted.)
struct PanelDsiHandle : lgfx::Panel_DSI {
  static constexpr esp_lcd_panel_handle_t lgfx::Panel_DSI::*handle = &PanelDsiHandle::_disp_panel_handle;
};
static void panel_free_second_fb(lgfx::Panel_DSI* panel) {
  esp_lcd_panel_handle_t h = panel->*PanelDsiHandle::handle;
  void *fb0 = nullptr, *fb1 = nullptr;
  if (h && esp_lcd_dpi_panel_get_frame_buffer(h, 2, &fb0, &fb1) == ESP_OK && fb0 == g_panel_fb && fb1 && fb1 != fb0)
    heap_caps_free(fb1);
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  auto cfg = M5.config();
  cfg.output_power = false;
  cfg.internal_imu = false;
  cfg.internal_mic = false;
  cfg.internal_spk = true;   // configured here, switched on only while there is sound (audio.h)
  M5.begin(cfg);
  audio_setup();
  xSemaphoreTake(g_i2c_lock, portMAX_DELAY);
  clock_setup();   // (before the PC: its CMOS clock reads the time)
  xSemaphoreGive(g_i2c_lock);
  Serial.printf("\nTab5 PC %s (reset reason %d, wakeup cause %d)\n", FIRMWARE_VERSION, (int)esp_reset_reason(),
                (int)esp_sleep_get_wakeup_cause());
  kbd_begin();
  usb_kbd_setup();
  g_rot = kbd_present_at_boot() ? 3 : 1;   // docked in the keyboard, the Tab5 is upside down
  M5.Display.setRotation(g_rot);
  g_brightness = g_prefs.getUChar("bri", 160);
  M5.Display.setBrightness(g_brightness);

  auto* panel = static_cast<lgfx::Panel_DSI*>(M5.Display.getPanel());
  g_panel_fb = panel ? (uint16_t*)panel->config_detail().buffer : nullptr;
  if (panel) panel_free_second_fb(panel);
  g_emu_fb = (uint16_t*)heap_caps_aligned_calloc(128, 1, (size_t)EMU_W * EMU_H * 2, MALLOC_CAP_SPIRAM);
  ppa_client_config_t pc = {};
  pc.oper_type = PPA_OPERATION_SRM;
  if (!g_panel_fb || !g_emu_fb || ppa_register_client(&pc, &g_ppa) != ESP_OK) {
    show_status("Hardware setup failed", "panel frame buffer / PPA / memory");
    for (;;) delay(1000);
  }

  g_side_on = g_prefs.getBool("side", true);
  ui_setup();
  side_setup();
  g_mouse_gain = g_prefs.getFloat("mouse", 1.0f);
  g_machine = g_prefs.getString("machine", "");   // (linux_install.h)
  show_status("Tab5 PC", "Starting...");
  const bool have_sd = sd_mount();
  if (have_sd) Serial.printf("SD card: %llu MB\n", SD_MMC.cardSize() >> 20);
  else Serial.println("SD card: none");
  if (have_sd) install_mode_run();   // (an install asked for: done instead of a PC; the Tab5 restarts after)
  load_config(g_conf, have_sd);
  if (!have_sd) {
    show_status("No SD card", "Starting the PC without disks");
    delay(1500);
  }

  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.display();
  fb_sync();
  g_input = xQueueCreate(64, sizeof(InputEvent));
  g_pc = pc_new(redraw, nullptr, (u8*)g_emu_fb, &g_conf);
  t386_shutdown = [] { g_pc->reset_request = 1; };   // (a triple fault: the PC resets, as a board does)
  t386_lfb_hooks();
  guest_net_mac();
  guest_net_start();   // (the PC's network, Wi-Fi on or off)
  t386_vga = g_pc->vga;
  t386_vga_fw_update();
  g_vga_shadow_size = min<size_t>(g_conf.vga_mem_size, 0x40000);
  g_vga_shadow = (uint8_t*)heap_caps_malloc(g_vga_shadow_size, MALLOC_CAP_SPIRAM);
  // the VBE modes' buffer, leaving 1.5 MB for what comes after (disks, sound, network, the web page)
  if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= NATIVE_VBE_W * NATIVE_VBE_H * 2 + (3 << 19))
    g_native_fb = (uint16_t*)heap_caps_aligned_calloc(128, 1, NATIVE_VBE_W * NATIVE_VBE_H * 2, MALLOC_CAP_SPIRAM);
  if (g_native_fb) {
    g_native_w = t386_vbe_max_w = NATIVE_VBE_W;
    g_native_h = t386_vbe_max_h = NATIVE_VBE_H;
  } else {
    g_native_fb = (uint16_t*)heap_caps_aligned_calloc(128, 1, NATIVE_W * NATIVE_H * 2, MALLOC_CAP_SPIRAM);
  }
  Serial.printf("VBE modes up to %d x %d (PSRAM free %u)\n", g_native_w, g_native_h,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  if (!g_vga_shadow) Serial.println("no memory for the video memory copy: the screen may tear");
  disk_raw_setup(g_conf);
  load_bios_and_reset(g_pc);
  g_pc->boot_start_time = get_uticks();
  audio_start();
  disk_setup();
  wifi_setup();
  xTaskCreatePinnedToCore(display_task, "display", 8192, nullptr, 2, nullptr, 0);
}

// Core 1: the PC itself, flat out (this core's idle task isn't watched by the task watchdog).
void loop() {
  for (;;) {
    uint32_t c0, n0, c, n;   // the CPU's counters (32 bits wrap in 12 s: added up step by step)
    asm volatile("csrr %0, mcycle" : "=r"(c0));
    asm volatile("csrr %0, minstret" : "=r"(n0));
    if (g_hold || g_pc_off) {   // paused (pc_hold), or off
      g_held = true;
      delay(5);
      continue;
    }
    g_held = false;
    if (uxQueueMessagesWaiting(g_input)) input_deliver();
    if (g_gnet_rx && uxQueueMessagesWaiting(g_gnet_rx)) guest_net_deliver();
    if (g_install_mode && g_pc->reset_request && fd_install_finish()) {   // an install done (fd_install.h): restart
      delay(500);
      tab5_restart();
    }
    if (g_pc->reset_request && !g_cold_reset) {   // the guest asked for a reset (port 92h, the keyboard controller):
      // where it was, for serial "cpu" (a program that crashed into a reboot)
      g_reset_count++;
      g_reset_ms = millis();
      t386_cpu_regs(g_pc->cpu, g_reset_regs, sizeof(g_reset_regs));
    }
    if (g_cold_reset) {   // (not the 640K-1M hole: tiny386 keeps device state there; the BIOS is loaded anew)
      g_cold_reset = false;
      memset(g_pc->phys_mem, 0, 0xa0000);
      if (g_pc->phys_mem_size > 0x100000) memset(g_pc->phys_mem + 0x100000, 0, g_pc->phys_mem_size - 0x100000);
      t386_ram_written(0, g_pc->phys_mem_size);   // (the translator's blocks there go)
      g_pc->reset_request = 1;
    }
    pc_step(g_pc);
    if (g_pc->shutdown_state == 8 && !g_pc_off) {   // the OS turned it off (APM): off, as by its power button
      g_pc_off = true;
      Serial.println("the PC powered off");
    }
    asm volatile("csrr %0, mcycle" : "=r"(c));
    asm volatile("csrr %0, minstret" : "=r"(n));
    g_hpc_cyc += c - c0;
    g_hpc_ins += n - n0;
  }
}
