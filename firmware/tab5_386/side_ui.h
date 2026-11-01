// The side panels beside the PC's picture (landscape x < OUT_X and x >= OUT_X + OUT_W: 160 pixels each, which the
// picture, always 4:3, never covers), as on a PC's case: buttons on the left, lights on the right. The settings
// panel turns them off (g_side_on: the margins black, touches there the touchpad's).
//   power     held SIDE_HOLD_MS: the PC off, as by its switch (g_pc_off: held, its picture gone, no sound); a tap
//             turns it on again (a cold start). It goes off too when the OS powers it off (APM: tiny386's shutdown
//             port, pc->shutdown_state 8). Stopped from the web page (g_pc_stopped), a tap restarts the Tab5.
//             (Without the side panels a tap anywhere turns an off PC on.)
//   reset     held SIDE_HOLD_MS: a cold start (as the settings panel's Reset PC); a ring fills around the icon
//   pause     a tap: paused (the backlight dimmed, no sound), a tap again: on
//   setup     a tap: the settings panel
//   lights    power (green on, amber paused, dim green off), disk (amber: the PC's disk used lately; dim when not),
//             the battery (grey; red at 15% or less)
// Each panel is an LVGL display of its own (SIDE_W x SCR_H, anti-aliased), drawn where something changed and put on
// the panel by the PPA (settings_ui.h's ppa_blit), only while the settings panel is closed. Drawn at once when
// something changed (lv_refr_now), not by LVGL's timers: nothing here moves by itself, and LVGL run every 10 ms
// while the settings panel was closed made a game's timed run 10% slower in one build. The touches are taken
// here (touch.h), not through LVGL: a button's look follows them.
#pragma once

static constexpr int SIDE_W = OUT_X, SIDE_HOLD_MS = 1000;
static constexpr int64_t SIDE_DISK_MS = 120;   // the disk light stays on this long after the disk was used
enum { SB_POWER, SB_RESET, SB_PAUSE, SB_SETUP, SB_N };
static constexpr int SB_SIZE = 112;
static int sb_y(int i) { return 104 + i * 170; }   // a button's centre

// the case: dark grey, a dark seam toward the picture (a light one there met the picture's text); keys raised on it (a light top edge, a shadow), lights in
// recessed windows. Flat colours that RGB565 holds exactly (green steps twice as fine as red and blue: in between,
// a gradient's greys came out banded, tinted green and red; LVGL 9 doesn't dither).
static constexpr uint32_t K_CASE = 0x202428, K_EDGE = 0x080C10, K_KEY = 0x383C40, K_KEY_RIM = 0x181C20,
                          K_KEY_LIGHT = 0x404448, K_KEY_DOWN = 0x282C30, K_INK = 0xDDE2E8, K_LABEL = 0x9AA2AC,
                          K_INK_OFF = 0x575D66, K_WELL = 0x101418, K_WELL_RIM = 0x303438, K_GREEN = 0x3DDC84,
                          K_AMBER = 0xFFB300, K_RED = 0xF0524F;

struct SideState {   // what is shown
  bool pressed[SB_N], enabled[SB_N], paused, off;
  int ring[SB_N];    // a hold's progress, percent
  int power;         // the power light: 0 off, 1 on, 2 paused
  bool disk;
  int batt_state, batt_level;   // BatteryPresence state, the level
  bool batt_charging;
};
static SideState g_side_shown;
static bool g_side_made = false, g_side_fresh = true;   // fresh: every part set again (just made)
static lv_display_t* g_side_disp[2];
static lv_obj_t *g_sb_key[SB_N], *g_sb_icon[SB_N], *g_sb_text[SB_N], *g_sb_ring[SB_N];
static lv_obj_t *g_sl_power, *g_sl_disk, *g_sl_batt_icon, *g_sl_batt_text;
static int g_side_pressed = -1;    // the button under the finger (SB_N: anywhere, with the side panels off)
static int64_t g_side_press_at = 0;
static bool g_side_fired = false;  // the held button's action is done (until the finger lifts)
static uint32_t g_side_disk_seen = 0;
static int64_t g_side_disk_until = 0;
static int g_side_redraw = 0;      // the side panels to draw again in full (bit 0 left, 1 right)
static uint8_t g_side_bri = 0, g_side_bri_seen = 0;
static int g_stap_x, g_stap_y;     // serial "stap x y ms": a press on the side panels held ms (released then)
static int64_t g_stap_until = 0;
static void side_touch(int x, int y, int kind, int64_t now);

// ---- the LVGL displays

static void side_flush(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
  if (g_side_on && !g_panel_open)
    ppa_blit(px, (d == g_side_disp[0] ? 0 : SCR_W - SIDE_W) + a->x1, a->y1, lv_area_get_width(a), lv_area_get_height(a));
  lv_display_flush_ready(d);
}

static void side_invalidate() {   // (the margins were cleared: back from the settings panel, or a status screen)
  if (g_side_made)
    for (lv_display_t* d : g_side_disp) lv_obj_invalidate(lv_display_get_screen_active(d));
  g_side_redraw = 3;
}

static lv_obj_t* side_box(lv_obj_t* parent, int x, int y, int w, int h) {   // a plain object, no theme's styles
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

static lv_obj_t* side_text(lv_obj_t* parent, const lv_font_t* font, uint32_t color, int spacing) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_remove_style_all(l);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_set_style_text_letter_space(l, spacing, 0);
  return l;
}

static lv_obj_t* side_well(lv_obj_t* parent, int x, int y, int w, int h) {   // a recessed window for lights
  lv_obj_t* o = side_box(parent, x, y, w, h);
  lv_obj_set_style_radius(o, 18, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(K_WELL), 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_border_color(o, lv_color_hex(K_WELL_RIM), 0);
  return o;
}

static lv_obj_t* side_led(lv_obj_t* parent, int x, int y, const char* name) {
  lv_obj_t* led = lv_led_create(parent);
  lv_obj_remove_style_all(led);
  lv_obj_set_pos(led, x, y);
  lv_obj_set_size(led, 18, 18);
  lv_obj_set_style_radius(led, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(led, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(led, 1, 0);
  lv_obj_set_style_border_opa(led, LV_OPA_40, 0);
  lv_obj_set_style_shadow_width(led, 16, 0);   // the glow (lv_led scales it by the brightness)
  lv_obj_set_style_shadow_spread(led, 2, 0);
  lv_obj_set_style_shadow_opa(led, LV_OPA_COVER, 0);
  lv_obj_t* l = side_text(parent, &lv_font_montserrat_14, K_LABEL, 1);
  lv_label_set_text(l, name);
  lv_obj_set_pos(l, x + 30, y + 1);
  return led;
}

static void side_key(lv_obj_t* scr, int i) {
  static lv_style_t key, down;
  static bool styled = false;
  if (!styled) {
    styled = true;
    lv_style_init(&key);
    lv_style_set_radius(&key, 22);
    lv_style_set_bg_opa(&key, LV_OPA_COVER);
    lv_style_set_bg_color(&key, lv_color_hex(K_KEY));
    lv_style_set_border_width(&key, 1);   // light from above: its top edge, a shade lighter
    lv_style_set_border_side(&key, LV_BORDER_SIDE_TOP);
    lv_style_set_border_color(&key, lv_color_hex(K_KEY_LIGHT));
    lv_style_set_outline_width(&key, 1);
    lv_style_set_outline_color(&key, lv_color_hex(K_KEY_RIM));
    lv_style_set_shadow_width(&key, 18);
    lv_style_set_shadow_offset_y(&key, 6);
    lv_style_set_shadow_color(&key, lv_color_black());
    lv_style_set_shadow_opa(&key, LV_OPA_60);
    lv_style_init(&down);   // pressed in: darker, lower, its shadow and top light nearly gone
    lv_style_set_bg_color(&down, lv_color_hex(K_KEY_DOWN));
    lv_style_set_border_color(&down, lv_color_hex(K_KEY));
    lv_style_set_shadow_width(&down, 6);
    lv_style_set_shadow_offset_y(&down, 1);
    lv_style_set_translate_y(&down, 3);
  }
  static const char* const icons[SB_N] = {LV_SYMBOL_POWER, LV_SYMBOL_REFRESH, LV_SYMBOL_PAUSE, LV_SYMBOL_SETTINGS};
  static const char* const names[SB_N] = {"POWER", "RESET", "PAUSE", "SETUP"};
  lv_obj_t* b = g_sb_key[i] = side_box(scr, (SIDE_W - SB_SIZE) / 2, sb_y(i) - SB_SIZE / 2, SB_SIZE, SB_SIZE);
  lv_obj_add_style(b, &key, 0);
  lv_obj_add_style(b, &down, LV_STATE_PRESSED);
  if (i == SB_POWER || i == SB_RESET) {   // the hold's ring, around the icon
    lv_obj_t* r = g_sb_ring[i] = lv_arc_create(b);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, 78, 78);
    lv_obj_align(r, LV_ALIGN_CENTER, 0, -12);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_bg_angles(r, 0, 360);
    lv_arc_set_rotation(r, 270);
    lv_arc_set_range(r, 0, 100);
    lv_arc_set_value(r, 0);
    lv_obj_set_style_arc_width(r, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(r, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(r, lv_color_hex(i == SB_POWER ? K_RED : K_AMBER), LV_PART_INDICATOR);
    lv_obj_add_flag(r, LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_t* ic = g_sb_icon[i] = side_text(b, &lv_font_montserrat_36, K_INK, 0);
  lv_label_set_text(ic, icons[i]);
  lv_obj_align(ic, LV_ALIGN_CENTER, 0, -12);
  lv_obj_t* t = g_sb_text[i] = side_text(b, &lv_font_montserrat_14, K_LABEL, 2);
  lv_label_set_text(t, names[i]);
  lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -12);
}

static void side_setup() {   // after ui_setup (lv_init, the settings panel's display: the default one again after)
  for (int k = 0; k < 2; ++k) {
    lv_display_t* d = g_side_disp[k] = lv_display_create(SIDE_W, SCR_H);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(d, side_flush);
    const size_t buf = SIDE_W * 120 * 2;
    lv_display_set_buffers(d, ui_alloc(buf), nullptr, buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_obj_t* scr = lv_display_get_screen_active(d);
    lv_obj_remove_style_all(scr);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(K_CASE), 0);
    lv_obj_t* edge = side_box(scr, k ? 0 : SIDE_W - 6, 0, 6, SCR_H);   // toward the picture: room for its text
    lv_obj_set_style_bg_opa(edge, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(edge, lv_color_hex(K_EDGE), 0);
    if (k == 0) {
      for (int i = 0; i < SB_N; ++i) side_key(scr, i);
    } else {
      lv_obj_t* w = side_well(scr, 16, 32, SIDE_W - 32, 132);
      g_sl_power = side_led(w, 18, 34, "POWER");
      g_sl_disk = side_led(w, 18, 80, "DISK");
      lv_obj_t* bw = side_well(scr, 16, SCR_H - 32 - 128, SIDE_W - 32, 128);
      g_sl_batt_icon = side_text(bw, &lv_font_montserrat_36, K_LABEL, 0);
      lv_obj_align(g_sl_batt_icon, LV_ALIGN_CENTER, 0, -18);
      g_sl_batt_text = side_text(bw, &lv_font_montserrat_20, K_LABEL, 1);
      lv_obj_align(g_sl_batt_text, LV_ALIGN_CENTER, 0, 30);
    }
  }
  lv_display_set_default(g_disp);
  g_side_made = true;
}

// ---- state and actions

static void pc_power_on() {   // a cold start (loop() does it before its next step)
  if (!g_pc_off) return;
  g_cold_reset = true;
  g_pc->shutdown_state = 0;
  g_pc_off = false;
  g_redraw_pc = true;   // (the PC's picture, and the margins)
  Serial.println("the PC powered on");
}

static void pc_power_off() {
  if (g_pc_off) return;
  if (g_user_pause) pc_set_paused(false);
  g_pc_off = true;
  Serial.println("the PC powered off (power button)");
}

static SideState side_wanted(int64_t now) {
  SideState s = {};
  const bool on = !g_pc_off && !g_pc_stopped;
  s.paused = g_user_pause;
  s.off = !on;
  for (int i = 0; i < SB_N; ++i) {
    s.pressed[i] = g_side_pressed == i;
    s.enabled[i] = i == SB_POWER || i == SB_SETUP || on;
  }
  if (g_side_pressed >= 0 && g_side_pressed < SB_N && !g_side_fired && on &&
      (g_side_pressed == SB_POWER || g_side_pressed == SB_RESET))
    s.ring[g_side_pressed] = (int)min<int64_t>(100, (now - g_side_press_at) * 100 / SIDE_HOLD_MS);
  s.power = !on ? 0 : g_user_pause || g_disk.drive >= 0 ? 2 : 1;
  const uint32_t used = t386_disk_cmds + t386_disk_hits;
  if (used != g_side_disk_seen) {
    g_side_disk_seen = used;
    g_side_disk_until = now + SIDE_DISK_MS;
  }
  s.disk = on && now < g_side_disk_until;
  s.batt_state = g_batt.presence.state;
  s.batt_level = g_batt.level;
  s.batt_charging = g_batt.charging;
  return s;
}

static void side_light(lv_obj_t* led, uint32_t color, bool lit) {   // off: its colour dim, as a real LED's lens
  lv_led_set_color(led, lv_color_hex(color));
  if (lit) lv_led_on(led); else lv_led_off(led);
}

// The LVGL objects brought up to date (only what changed); which panels changed (bit 0 left, 1 right)
static int side_show(const SideState& s) {
  SideState& was = g_side_shown;
  const bool all = g_side_fresh;
  g_side_fresh = false;
  int changed = 0;
  for (int i = 0; i < SB_N; ++i) {
    if (all || s.pressed[i] != was.pressed[i]) {
      changed |= 1;
      if (s.pressed[i]) lv_obj_add_state(g_sb_key[i], LV_STATE_PRESSED);
      else lv_obj_remove_state(g_sb_key[i], LV_STATE_PRESSED);
    }
    if (all || s.enabled[i] != was.enabled[i] || s.off != was.off) {   // (off: POWER green, to turn it on)
      changed |= 1;
      const uint32_t ink = !s.enabled[i] ? K_INK_OFF : i == SB_POWER && s.off ? K_GREEN : K_INK;
      lv_obj_set_style_text_color(g_sb_icon[i], lv_color_hex(ink), 0);
      lv_obj_set_style_text_color(g_sb_text[i], lv_color_hex(s.enabled[i] ? K_LABEL : K_INK_OFF), 0);
    }
    if (g_sb_ring[i] && (all || s.ring[i] != was.ring[i])) {
      changed |= 1;
      if (s.ring[i] > 0) lv_obj_remove_flag(g_sb_ring[i], LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(g_sb_ring[i], LV_OBJ_FLAG_HIDDEN);
      lv_arc_set_value(g_sb_ring[i], s.ring[i]);
    }
  }
  if (all || s.paused != was.paused) {
    changed |= 1;
    lv_label_set_text(g_sb_icon[SB_PAUSE], s.paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    lv_label_set_text(g_sb_text[SB_PAUSE], s.paused ? "RESUME" : "PAUSE");
  }
  if (all || s.power != was.power || s.disk != was.disk) changed |= 2;
  if (all || s.power != was.power) side_light(g_sl_power, s.power == 2 ? K_AMBER : K_GREEN, s.power != 0);
  if (all || s.disk != was.disk) side_light(g_sl_disk, K_AMBER, s.disk);
  if (all || s.batt_state != was.batt_state || s.batt_level != was.batt_level || s.batt_charging != was.batt_charging) {
    changed |= 2;
    char text[16];
    const char* icon = LV_SYMBOL_USB;
    uint32_t color = K_LABEL;
    if (s.batt_state == BatteryPresence::Present) {
      const int l = s.batt_level;
      icon = l > 85 ? LV_SYMBOL_BATTERY_FULL : l > 60 ? LV_SYMBOL_BATTERY_3 : l > 35 ? LV_SYMBOL_BATTERY_2
           : l > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
      if (l <= 15) color = K_RED;   // else grey: only a low battery stands out
      snprintf(text, sizeof(text), "%s%d%%", s.batt_charging ? LV_SYMBOL_CHARGE " " : "", l);
    } else {
      snprintf(text, sizeof(text), s.batt_state == BatteryPresence::Absent ? "USB" : "...");
    }
    lv_label_set_text(g_sl_batt_icon, icon);
    lv_obj_set_style_text_color(g_sl_batt_icon, lv_color_hex(color), 0);
    lv_label_set_text(g_sl_batt_text, text);
    lv_obj_align(g_sl_batt_text, LV_ALIGN_CENTER, 0, 30);
  }
  was = s;
  return changed;
}

// From the display task, every round: the actions of held buttons, the backlight while paused, the sound, and the
// side panels brought up to date and drawn where they changed (while the settings panel is open: after it)
static void side_poll(int64_t now) {
  if (g_stap_until && now >= g_stap_until) {
    g_stap_until = 0;
    side_touch(g_stap_x, g_stap_y, 0, now);
  }
  const bool on = !g_pc_off && !g_pc_stopped;
  if (g_side_pressed >= 0 && !g_side_fired && on && now - g_side_press_at >= SIDE_HOLD_MS) {
    g_side_fired = true;
    if (g_side_pressed == SB_POWER) {
      pc_power_off();
    } else if (g_side_pressed == SB_RESET) {
      if (g_user_pause) pc_set_paused(false);
      pc_cold_reset();
      Serial.println("the PC reset (reset button)");
    }
  }
  g_audio_quiet = !on || g_user_pause || g_disk.drive >= 0;
  const uint8_t bri = g_user_pause && on && !g_panel_open ? max(8, g_brightness / 4) : g_brightness;
  if (bri != g_side_bri || g_brightness != g_side_bri_seen) {
    g_side_bri = bri;
    g_side_bri_seen = g_brightness;
    M5.Display.setBrightness(bri);
  }
  if (!g_side_made || g_panel_open) return;
  const int redraw = side_show(side_wanted(now)) | g_side_redraw;
  g_side_redraw = 0;
  for (int k = 0; k < 2; ++k)
    if (redraw & 1 << k) lv_refr_now(g_side_disp[k]);
}

// ---- touches (touch.h): kind 1 down, 2 moved, 0 up

static bool side_owns(int x, int y) {   // a touch starting there is the side panels' (all of them while the PC is off)
  if (y < EDGE_PX) return false;   // (the top edge: a swipe down opens the settings panel)
  return g_pc_off || g_pc_stopped || (g_side_on && (x < OUT_X || x >= OUT_X + OUT_W));
}

static int side_button_at(int x, int y) {
  if (!g_side_on || x >= SIDE_W) return -1;
  for (int i = 0; i < SB_N; ++i)
    if (abs(y - sb_y(i)) <= SB_SIZE / 2 + 8) return i;
  return -1;
}

static void side_touch(int x, int y, int kind, int64_t now) {
  if (kind == 1) {
    g_side_pressed = side_button_at(x, y);
    g_side_press_at = now;
    g_side_fired = false;
    if (g_side_pressed < 0 && !g_side_on) g_side_pressed = SB_N;   // (no side panels: anywhere, to turn it on)
    return;
  }
  if (g_side_pressed < 0) return;
  if (kind == 2) {   // slid off the button: nothing
    if (g_side_pressed < SB_N && side_button_at(x, y) != g_side_pressed) g_side_pressed = -1;
    return;
  }
  const int i = g_side_pressed;
  g_side_pressed = -1;
  if (g_side_fired) return;
  const bool on = !g_pc_off && !g_pc_stopped;
  if ((i == SB_POWER || i == SB_N) && g_pc_stopped) restart_tab5();   // (stopped from the web page: as Start PC)
  else if ((i == SB_POWER || i == SB_N) && g_pc_off) pc_power_on();
  else if (i == SB_PAUSE && on) pc_set_paused(!g_user_pause);
  else if (i == SB_SETUP) panel_open();
}
