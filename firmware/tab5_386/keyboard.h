// The Tab5 Keyboard (M5Stack A164): an STM32 on Ext.Port1 that scans a 5 x 14 key matrix and queues press/release
// events, read over I2C (address 0x6D, SDA G0, SCL G1; INT G50 goes low while events are queued).
// Each key maps to a USB HID usage code (plus a Shift it needs, for the
// symbols printed on their own keys), which goes to the emulated PC's PS/2 keyboard as a Linux key code.
// Aa, Ctrl and Alt are the PC's left Shift, Ctrl and Alt. Sym is a layer, never sent: the second symbol printed on a
// key, Home/End/Page Up/Page Down on the arrows, Delete on Backspace, and the keys DOS needs that the keyboard
// lacks: F1-F10 on 1-0, F11/F12 on - and +, Insert on Del, Caps Lock on Aa. Sym + Alt + Up/Down: speaker volume.
// Sym + Esc opens or closes the settings panel (settings_ui.h); while it is open, keys type into it, not the PC.
// Lights: the left one shows Sym / Aa held, the right one is green while the PC runs.
#pragma once

static constexpr uint8_t KBD_ADDR = 0x6D;
static constexpr int KBD_SDA = 0, KBD_SCL = 1, KBD_INT = 50;
static constexpr uint32_t KBD_FREQ = 400000;
static constexpr int64_t KBD_PROBE_MS = 2000;   // look for a keyboard this often while none answers
static constexpr int64_t KBD_POLL_MS = 100;     // read the queue at least this often, should an INT edge be missed
enum : uint8_t { KBD_REG_EVENT_NUM = 0x02, KBD_REG_BRIGHTNESS = 0x03, KBD_REG_MODE = 0x10, KBD_REG_RGB_MODE = 0x11,
                 KBD_REG_RGB1 = 0x60, KBD_REG_RGB2 = 0x64, KBD_REG_KEY_EVENT = 0x20, KBD_REG_VERSION = 0xFE };
static constexpr uint8_t KMOD_SHIFT = 0x02;   // HID modifier bit: the key needs Shift

// HID usage code and forced Shift of each key, row by row, from M5Stack's M5Unit-KEYBOARD library
// (https://github.com/m5stack/M5Unit-KEYBOARD): MIT License, Copyright (c) 2025 M5Stack Technology CO LTD
// (LICENSES/MIT-M5Unit-KEYBOARD.txt).
// 0 = a modifier key: Sym (3,0), Aa (3,1), Ctrl (4,0), Alt (4,1).
struct KbdKey { uint8_t code, mod; };
static const KbdKey KBD_BASE[70] = {
  // Esc 1 2 3 4 5 6 7 8 9 0 - + Del
  {0x29, 0}, {0x1E, 0}, {0x1F, 0}, {0x20, 0}, {0x21, 0}, {0x22, 0}, {0x23, 0}, {0x24, 0}, {0x25, 0}, {0x26, 0},
  {0x27, 0}, {0x2D, 0}, {0x2E, 2}, {0x4C, 0},
  // ` ! @ # $ % ^ & * ( ) [ ] backslash
  {0x35, 0}, {0x1E, 2}, {0x1F, 2}, {0x20, 2}, {0x21, 2}, {0x22, 2}, {0x23, 2}, {0x24, 2}, {0x25, 2}, {0x26, 2},
  {0x27, 2}, {0x2F, 0}, {0x30, 0}, {0x31, 0},
  // Tab q w e r t y u i o p ; ' Backspace
  {0x2B, 0}, {0x14, 0}, {0x1A, 0}, {0x08, 0}, {0x15, 0}, {0x17, 0}, {0x1C, 0}, {0x18, 0}, {0x0C, 0}, {0x12, 0},
  {0x13, 0}, {0x33, 0}, {0x34, 0}, {0x2A, 0},
  // Sym Aa a s d f g h j k l Up _ Enter
  {0, 0}, {0, 0}, {0x04, 0}, {0x16, 0}, {0x07, 0}, {0x09, 0}, {0x0A, 0}, {0x0B, 0}, {0x0D, 0}, {0x0E, 0},
  {0x0F, 0}, {0x52, 0}, {0x2D, 2}, {0x28, 0},
  // Ctrl Alt z x c v b n m . Left Down Right Space
  {0, 0}, {0, 0}, {0x1D, 0}, {0x1B, 0}, {0x06, 0}, {0x19, 0}, {0x05, 0}, {0x11, 0}, {0x10, 0}, {0x37, 0},
  {0x50, 0}, {0x51, 0}, {0x4F, 0}, {0x2C, 0},
};
// The Sym layer differs only here: ` ~  ! ?  * /  ( <  ) >  [ {  ] }  \ |  ; :  ' "  _ =  . ,
// Left Home, Right End, Up Page Up, Down Page Down, Backspace Delete; 1-0 F1-F10, - F11, + F12, Del Insert.
struct KbdSym { uint8_t idx; KbdKey key; };
static const KbdSym KBD_SYM[] = {
  {14, {0x35, 2}}, {15, {0x38, 2}}, {22, {0x38, 0}}, {23, {0x36, 2}}, {24, {0x37, 2}}, {25, {0x2F, 2}},
  {26, {0x30, 2}}, {27, {0x31, 2}}, {39, {0x33, 2}}, {40, {0x34, 2}}, {54, {0x2E, 0}}, {65, {0x36, 0}},
  {66, {0x4A, 0}}, {68, {0x4D, 0}}, {53, {0x4B, 0}}, {67, {0x4E, 0}}, {41, {0x4C, 0}},
  {1, {0x3A, 0}}, {2, {0x3B, 0}}, {3, {0x3C, 0}}, {4, {0x3D, 0}}, {5, {0x3E, 0}}, {6, {0x3F, 0}},
  {7, {0x40, 0}}, {8, {0x41, 0}}, {9, {0x42, 0}}, {10, {0x43, 0}}, {11, {0x44, 0}}, {12, {0x45, 0}},
  {13, {0x49, 0}},
};
static constexpr uint8_t KIDX_SYM = 42, KIDX_AA = 43, KIDX_CTRL = 56, KIDX_ALT = 57;

// Linux input key codes (what ps2_put_keycode takes) of HID usage codes 0x00-0x52.
static const uint8_t HID_TO_LINUX[0x53] = {
  0, 0, 0, 0,
  30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,  // a-z
  2, 3, 4, 5, 6, 7, 8, 9, 10, 11,                        // 1-0
  28, 1, 14, 15, 57, 12, 13, 26, 27, 43, 43, 39, 40, 41, 51, 52, 53, 58,   // Enter Esc BS Tab Space - = [ ] \ # ; ' ` , . / Caps
  59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, 88,        // F1-F12
  99, 70, 119, 110, 102, 104, 111, 107, 109, 106, 105, 108, 103,   // PrtSc ScrLk Pause Ins Home PgUp Del End PgDn → ← ↓ ↑
};
static constexpr int LK_LSHIFT = 42, LK_LCTRL = 29, LK_LALT = 56, LK_CAPS = 58;

static bool g_panel_open = false;          // the settings panel is shown (settings_ui.h)
static void panel_toggle();
static void kbd_type_in_panel(KbdKey k);

struct Kbd {
  bool present = false;
  uint8_t version = 0;
  int fails = 0;                  // I2C errors in a row
  int64_t next_probe = 0, next_poll = 0;
  bool sym = false, aa = false, alt = false;
  KbdKey down[70] = {};           // what each held key was sent as, so its release matches even if Sym changed
  bool shift_added[70] = {};      // its press also pressed Shift (a symbol that needs it, without Aa held)
  uint32_t keys = 0;              // key events read, for the stats
  uint32_t led[2] = {~0u, ~0u};   // colour last set on each light (0xRRGGBB); ~0: unknown, set it again
};
static Kbd g_kbd;

static void pc_key(bool down, int linux_code) {
  if (g_pc && linux_code) input_put({0, (int8_t)down, (int16_t)linux_code, 0});
}

static void kbd_event(uint8_t raw) {
  const bool pressed = raw & 0x80;
  const int row = (raw >> 4) & 7, col = raw & 15;
  if (row > 4 || col > 13) return;
  const int i = row * 14 + col;
  ++g_kbd.keys;
  if (i == 0 && g_kbd.sym) {   // Sym + Esc: the settings panel
    if (pressed) panel_toggle();
    return;
  }
  const bool to_pc = !g_panel_open;
  switch (i) {
    case KIDX_SYM: g_kbd.sym = pressed; return;
    case KIDX_AA:
      if (g_kbd.sym && pressed) {   // Sym + Aa: Caps Lock (tapped)
        if (to_pc) { pc_key(true, LK_CAPS); pc_key(false, LK_CAPS); }
        return;
      }
      g_kbd.aa = pressed;
      if (to_pc) pc_key(pressed, LK_LSHIFT);
      return;
    case KIDX_CTRL: if (to_pc) pc_key(pressed, LK_LCTRL); return;
    case KIDX_ALT: g_kbd.alt = pressed; if (to_pc) pc_key(pressed, LK_LALT); return;
  }
  static constexpr int KIDX_UP = 53, KIDX_DOWN = 67;
  if (g_kbd.sym && g_kbd.alt && (i == KIDX_UP || i == KIDX_DOWN)) {   // the Tab5's volume, not a key for the PC
    if (pressed) set_volume(g_volume + (i == KIDX_UP ? 16 : -16));
    return;
  }
  if (!to_pc) {   // typing into the panel: Aa is Shift
    if (!pressed) return;
    KbdKey k = KBD_BASE[i];
    if (g_kbd.sym)
      for (const KbdSym& s : KBD_SYM)
        if (s.idx == i) k = s.key;
    if (g_kbd.aa) k.mod |= KMOD_SHIFT;
    kbd_type_in_panel(k);
    return;
  }
  if (pressed) {
    KbdKey k = KBD_BASE[i];
    if (g_kbd.sym)
      for (const KbdSym& s : KBD_SYM)
        if (s.idx == i) k = s.key;
    g_kbd.down[i] = k;
    g_kbd.shift_added[i] = (k.mod & KMOD_SHIFT) && !g_kbd.aa;
    if (g_kbd.shift_added[i]) pc_key(true, LK_LSHIFT);
    pc_key(true, k.code < sizeof(HID_TO_LINUX) ? HID_TO_LINUX[k.code] : 0);
  } else {
    const KbdKey k = g_kbd.down[i];
    g_kbd.down[i] = {0, 0};
    if (!k.code) return;   // pressed before we started reading
    pc_key(false, k.code < sizeof(HID_TO_LINUX) ? HID_TO_LINUX[k.code] : 0);
    if (g_kbd.shift_added[i] && !g_kbd.aa) pc_key(false, LK_LSHIFT);
    g_kbd.shift_added[i] = false;
  }
}

static bool kbd_probe() {
  if (!M5.Ex_I2C.scanID(KBD_ADDR, KBD_FREQ)) return false;
  g_kbd.version = M5.Ex_I2C.readRegister8(KBD_ADDR, KBD_REG_VERSION, KBD_FREQ);
  // normal mode (each key down and up), an empty queue (writing 0 to the count clears it), and the lights ours
  g_kbd.led[0] = g_kbd.led[1] = ~0u;
  return M5.Ex_I2C.writeRegister8(KBD_ADDR, KBD_REG_MODE, 0, KBD_FREQ) &&
         M5.Ex_I2C.writeRegister8(KBD_ADDR, KBD_REG_EVENT_NUM, 0, KBD_FREQ) &&
         M5.Ex_I2C.writeRegister8(KBD_ADDR, KBD_REG_RGB_MODE, 1, KBD_FREQ) &&
         M5.Ex_I2C.writeRegister8(KBD_ADDR, KBD_REG_BRIGHTNESS, 10, KBD_FREQ);
}

static constexpr uint32_t LED_OFF = 0, LED_WHITE = 0xFFFFFF, LED_AMBER = 0xFF8000, LED_GREEN = 0x00FF00;

static bool kbd_led(int i, uint32_t rgb) {
  if (g_kbd.led[i] == rgb) return true;
  const uint8_t bgr[3] = {(uint8_t)rgb, (uint8_t)(rgb >> 8), (uint8_t)(rgb >> 16)};   // registers: B, G, R
  const uint8_t reg = i ? KBD_REG_RGB2 : KBD_REG_RGB1;
  for (int c = 0; c < 3; ++c)
    if (!M5.Ex_I2C.writeRegister8(KBD_ADDR, reg + c, bgr[c], KBD_FREQ)) return false;
  g_kbd.led[i] = rgb;
  return true;
}

static bool kbd_leds(int64_t now) {
  const Kbd& k = g_kbd;
  const bool blink = (now / 400) % 2;
  const uint32_t left = k.aa && k.sym ? (blink ? LED_WHITE : LED_AMBER) : k.aa ? LED_WHITE : k.sym ? LED_AMBER : LED_OFF;
  const uint32_t right = g_pc && !g_pc_off ? LED_GREEN : LED_OFF;
  return kbd_led(0, left) && kbd_led(1, right);
}

static void kbd_release_all() {   // keys held when the keyboard went away
  for (int i = 0; i < 70; ++i)
    if (g_kbd.down[i].code) kbd_event(0x00 | (i / 14) << 4 | (i % 14));
  if (g_kbd.aa) pc_key(false, LK_LSHIFT);
  pc_key(false, LK_LCTRL);
  pc_key(false, LK_LALT);
  g_kbd.sym = g_kbd.aa = g_kbd.alt = false;
}

static void kbd_begin() {
  pinMode(KBD_INT, INPUT_PULLUP);
  // M5Unified's external bus (port 0) is otherwise for Port A, which this firmware doesn't use
  M5.Ex_I2C.begin(I2C_NUM_0, KBD_SDA, KBD_SCL);
}

static void kbd_poll() {
  Kbd& k = g_kbd;
  const int64_t now = esp_timer_get_time() / 1000;
  if (!k.present) {
    if (now < k.next_probe) return;
    k.next_probe = now + KBD_PROBE_MS;
    if (!kbd_probe()) return;
    k.present = true;
    k.fails = 0;
    Serial.printf("keyboard found (firmware 0x%02X)\n", k.version);
  }
  bool ok = kbd_leds(now);
  if (ok && digitalRead(KBD_INT) == HIGH && now < k.next_poll) return;
  k.next_poll = now + KBD_POLL_MS;
  uint8_t n = 0;
  ok = ok && M5.Ex_I2C.readRegister(KBD_ADDR, KBD_REG_EVENT_NUM, &n, 1, KBD_FREQ);
  for (int j = 0; ok && j < n && j < 32; ++j) {
    uint8_t raw = 0xFF;
    ok = M5.Ex_I2C.readRegister(KBD_ADDR, KBD_REG_KEY_EVENT, &raw, 1, KBD_FREQ);
    if (ok && raw != 0xFF) kbd_event(raw);
  }
  if (ok) { k.fails = 0; return; }
  if (++k.fails < 3) return;
  k.present = false;
  kbd_release_all();
  Serial.println("keyboard gone");
}

// At startup: is a keyboard fitted? Its controller powers up with the Tab5, so it gets a moment to answer.
static bool kbd_present_at_boot() {
  for (int i = 0; i < 5 && !g_kbd.present; ++i) {
    if (i) delay(100);
    g_kbd.next_probe = 0;
    kbd_poll();
  }
  return g_kbd.present;
}
