// The settings panel (LVGL): WiFi (on/off, network list, password dialog), the web address
// for copying files, the PC's controls (pause, reset), brightness, volume and touchpad speed.
// Opened by a swipe down from the top edge or Sym + Esc; closed by Done, Esc or Sym + Esc. While it is open the PC
// keeps running (sound plays) but its screen isn't drawn, touches go to the panel, and keys type into it.
// LVGL only runs on the core-0 task, and only while the panel is open; it draws into strips that the PPA rotates
// into the panel's frame buffer.
// The look: a BIOS setup screen at today's resolution. VGA-style blue, frames of two 3-pixel rules with their names
// on the top rule, cyan bars for buttons and the key help at the bottom, and a pixel font (UNSCII 8, its pixels
// drawn 2 x 3 and 3 x 4 like a text mode's on a 4:3 monitor: src/px_fonts.c, tools/fonts/px_fonts.py).
#pragma once
#include <lvgl.h>

extern "C" const lv_font_t px_font_24, px_font_32;   // (16 x 24 and 24 x 32 cells)
static constexpr uint32_t C_BG = 0x0c1d9a, C_CARD = 0x0c1d9a, C_LINE = 0xe9e6d6, C_INK = 0xe9e6d6, C_DIM = 0x61e2e8,
                          C_ACCENT = 0xfff35c, C_WARN = 0xff7a6e, C_DEEP = 0x06104f, C_BAR = 0x2fbcc6, C_BLACK = 0x04060e,
                          C_KEY = 0xb9b7ad;
static constexpr int MAX_NETS = 24;
static constexpr int64_t RESET_CONFIRM_MS = 3000;

struct Net { char ssid[33]; int8_t rssi; bool secure; };
static Net g_nets[MAX_NETS];
static int g_net_count = 0;

static lv_display_t* g_disp;
static lv_obj_t *ui_wifi, *ui_addr, *ui_pcstate, *ui_list, *ui_scan_lbl, *ui_pause_lbl, *ui_reset_lbl;
static lv_obj_t *ui_modal, *ui_modal_title, *ui_ta_ssid, *ui_ta_pass, *ui_show_btn, *ui_msg, *ui_spinner, *ui_kb;
static lv_obj_t *ui_connect_btn, *ui_bri, *ui_vol, *ui_mouse, *ui_wifi_sw, *ui_batt, *ui_side_sw;
static lv_obj_t *ui_mach_dd, *ui_mach_start, *ui_inst_msg, *ui_dos_btn, *ui_lnx_btn, *ui_iso_btn;   // the machines
static int64_t g_start_armed = 0;   // "Start" tapped once at this time (as Reset PC: it restarts the Tab5)
// The dialog: a Wi-Fi network's name and password, or a new machine's name (Install DOS / Linux / ISO; for an ISO,
// the CD image too)
enum ModalMode { MODAL_WIFI, MODAL_DOS, MODAL_LINUX, MODAL_ISO };
static ModalMode g_modal_mode = MODAL_WIFI;
static lv_obj_t *ui_pass_row, *ui_iso_dd;
static String g_iso_name;   // the name the dialog suggested for the CD image chosen (replaced when another is chosen)
static String g_update_ok;   // the name Install was tapped for once (it warns that the PC stops): again installs
static bool g_scanning = false, g_connect_pending = false, g_touch_block = false;
static int64_t g_reset_armed = 0;   // "Reset PC" tapped once at this time: a second tap within RESET_CONFIRM_MS resets
// The Wi-Fi switch tapped: on (1) or off (0), done once the panel has shown it (ui_poll): switching on the first time
// after a restart takes ~2.5 s (the C6's link), with the panel standing still meanwhile. -1: nothing waiting.
// (Done WIFI_REQUEST_MS after the tap: the switch's knob moves in 120 ms; drawn at once, it stood half-way.)
static int g_wifi_request = -1;
static int64_t g_wifi_request_at;
static constexpr int WIFI_REQUEST_MS = 250;
// A scan asked for while connecting (it would fail then): done when the attempt has ended (ui_poll)
static bool g_scan_wanted = false;
static void ui_wifi_request_do();
static String g_pending_ssid;

static void panel_close();
static void side_cancel();   // (side_ui.h)

// ---- LVGL plumbing ------------------------------------------------------------------------------------------

// Rotate a w x h landscape RGB565 rectangle at logical (x, y) into the portrait frame buffer.
static void ppa_blit(const void* src, int x, int y, int w, int h) {
  JitHold hold;   // (tab5_386.ino: the write-back and the PPA's)
  esp_cache_msync((void*)src, (size_t)w * h * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  ppa_srm_oper_config_t op = {};
  op.in.buffer = src;
  op.in.pic_w = op.in.block_w = w;
  op.in.pic_h = op.in.block_h = h;
  op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  op.out.buffer = g_panel_fb;
  op.out.buffer_size = FB_W * FB_H * 2;
  op.out.pic_w = FB_W;
  op.out.pic_h = FB_H;
  op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  if (g_rot == 1) {           // logical (x, y) -> panel (719 - y, x)
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
    op.out.block_offset_x = FB_W - (y + h);
    op.out.block_offset_y = x;
  } else {                    // logical (x, y) -> panel (y, 1279 - x)
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
    op.out.block_offset_x = y;
    op.out.block_offset_y = FB_H - (x + w);
  }
  op.scale_x = op.scale_y = 1.0f;
  op.mode = PPA_TRANS_MODE_BLOCKING;
  ppa_do_scale_rotate_mirror(g_ppa, &op);
}

static void ui_flush(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
  // (LVGL runs while the panel is closed too, for the side panels: side_ui.h; panel_open draws it all anew)
  if (g_panel_open) ppa_blit(px, a->x1, a->y1, lv_area_get_width(a), lv_area_get_height(a));
  lv_display_flush_ready(d);
}

// A tap from serial "ptap x y" (the panel's coordinates, landscape): pressed for a few reads, then released
static int16_t g_fake_x, g_fake_y;
static int g_fake_reads = 0;

static void ui_touch(lv_indev_t*, lv_indev_data_t* d) {   // M5.update() has run in touch_poll()
  static int16_t lx = 0, ly = 0;
  if (!g_panel_open) {   // (the PC's touches: LVGL runs for the side panels as well)
    d->point.x = lx;
    d->point.y = ly;
    d->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  if (g_fake_reads > 0) {
    d->point.x = lx = g_fake_x;
    d->point.y = ly = g_fake_y;
    d->state = --g_fake_reads > 0 ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    return;
  }
  bool pressed = M5.Touch.getCount() > 0;
  if (pressed) {
    const auto& t = M5.Touch.getDetail(0);
    lx = t.x;
    ly = t.y;
  }
  if (g_touch_block) {          // the finger that opened the panel: ignore it until it lifts
    if (!pressed) g_touch_block = false;
    pressed = false;
  }
  d->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  d->point.x = lx;
  d->point.y = ly;
}

static void* ui_alloc(size_t bytes) { return heap_caps_aligned_alloc(128, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

// ---- small builders -----------------------------------------------------------------------------------------

static lv_obj_t* ui_card(lv_obj_t* parent, int x, int y, int w, int h) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, lv_color_hex(C_CARD), 0);
  lv_obj_set_style_border_color(o, lv_color_hex(C_LINE), 0);   // the double rule: the border, and an outline outside it
  lv_obj_set_style_border_width(o, 3, 0);
  lv_obj_set_style_outline_color(o, lv_color_hex(C_LINE), 0);
  lv_obj_set_style_outline_width(o, 3, 0);
  lv_obj_set_style_outline_pad(o, 3, 0);
  lv_obj_set_style_radius(o, 0, 0);
  lv_obj_set_style_pad_all(o, 22, 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

// A frame's name, on its top rule (centred over a frame from x to x + w, its rule at y), on the screen
static lv_obj_t* ui_caption(lv_obj_t* scr, const char* text, int x, int y, int w) {
  lv_obj_t* l = lv_label_create(scr);
  lv_label_set_text_fmt(l, " %s ", text);
  lv_obj_set_style_text_font(l, &px_font_24, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(C_INK), 0);
  lv_obj_set_style_bg_color(l, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
  lv_obj_update_layout(l);
  lv_obj_set_pos(l, x + (w - lv_obj_get_width(l)) / 2, y - 14);
  return l;
}

static lv_obj_t* ui_label(lv_obj_t* parent, const char* text, const lv_font_t* font, uint32_t color) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  return l;
}

static lv_obj_t* ui_button(lv_obj_t* parent, const char* text, lv_event_cb_t cb, bool primary) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_style_bg_color(b, lv_color_hex(primary ? C_ACCENT : C_BAR), 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_INK), LV_STATE_PRESSED);
  lv_obj_set_style_radius(b, 0, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t* l = ui_label(b, text, &px_font_24, C_BLACK);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  return b;
}

static lv_obj_t* ui_slider(lv_obj_t* parent, const char* name, int y, int lo, int hi, lv_event_cb_t changed) {
  lv_obj_t* l = ui_label(parent, name, &px_font_24, C_INK);
  lv_obj_set_pos(l, 0, y);
  lv_obj_t* s = lv_slider_create(parent);
  lv_slider_set_range(s, lo, hi);
  lv_obj_set_size(s, 300, 20);
  lv_obj_set_pos(s, 190, y + 2);
  lv_obj_set_ext_click_area(s, 12);   // (the sliders are 44 apart)
  lv_obj_set_style_radius(s, 0, LV_PART_MAIN);   // a bar in a box, a square knob
  lv_obj_set_style_radius(s, 0, LV_PART_INDICATOR);
  lv_obj_set_style_radius(s, 0, LV_PART_KNOB);
  lv_obj_set_style_bg_color(s, lv_color_hex(C_DEEP), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(s, lv_color_hex(C_LINE), LV_PART_MAIN);
  lv_obj_set_style_border_width(s, 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, lv_color_hex(C_INK), LV_PART_KNOB);
  lv_obj_set_style_pad_all(s, 6, LV_PART_KNOB);
  lv_obj_add_event_cb(s, changed, LV_EVENT_VALUE_CHANGED, nullptr);
  return s;
}

// A square on/off switch: a box with a block that moves
static void ui_switch_style(lv_obj_t* sw) {
  lv_obj_set_size(sw, 80, 36);
  lv_obj_set_ext_click_area(sw, 16);   // (a small target on a 5-inch screen)
  lv_obj_set_style_radius(sw, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(sw, 0, LV_PART_INDICATOR);
  lv_obj_set_style_radius(sw, 0, LV_PART_KNOB);
  lv_obj_set_style_bg_color(sw, lv_color_hex(C_DEEP), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(sw, lv_color_hex(C_LINE), LV_PART_MAIN);
  lv_obj_set_style_border_width(sw, 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sw, lv_color_hex(C_BAR), (lv_style_selector_t)LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(sw, lv_color_hex(C_INK), LV_PART_KNOB);
  lv_obj_set_style_pad_all(sw, -5, LV_PART_KNOB);
}

// A switch's label toggles it too
static void ui_switch_label(lv_obj_t* label, lv_obj_t* sw) {
  lv_obj_add_flag(label, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(label, 12);
  lv_obj_add_event_cb(label, [](lv_event_t* e) {
    lv_obj_t* s = (lv_obj_t*)lv_event_get_user_data(e);
    if (lv_obj_has_state(s, LV_STATE_CHECKED)) lv_obj_remove_state(s, LV_STATE_CHECKED);
    else lv_obj_add_state(s, LV_STATE_CHECKED);
    lv_obj_send_event(s, LV_EVENT_VALUE_CHANGED, nullptr);
  }, LV_EVENT_CLICKED, sw);
}

// ---- machines -----------------------------------------------------------------------------------------------

// The card's machines into the list (linux_install.h), the one running chosen; and the CD images there (fd_install.h)
static void ui_fill_machines() {
  machines_scan();
  fd_find_isos();
  String opts;
  int cur = 0;
  for (int i = 0; i < g_machine_count; ++i) {
    if (i) opts += "\n";
    opts += g_machines[i].name;
    if (machine_path() == "/sdcard/" + String(g_machines[i].file)) cur = i;
  }
  lv_dropdown_set_options(ui_mach_dd, g_machine_count ? opts.c_str() : "(none yet)");
  lv_dropdown_set_selected(ui_mach_dd, cur);
  g_start_armed = 0;
}

// ---- status -------------------------------------------------------------------------------------------------

static void ui_refresh_status() {
  if (g_wifi_request >= 0) {   // (the switch as tapped; ui_poll does it)
    lv_label_set_text(ui_wifi, g_wifi_request ? "Switching Wi-Fi on ..." : "Switching Wi-Fi off ...");
    lv_obj_set_style_text_color(ui_wifi, lv_color_hex(C_INK), 0);
  } else switch (g_wifi) {
    case WIFI_DISABLED:        lv_label_set_text(ui_wifi, "Wi-Fi is off: switch it on to copy files"); break;
    case WIFI_NONE:       lv_label_set_text(ui_wifi, "Not set up - choose a network"); break;
    case WIFI_CONNECTING: lv_label_set_text_fmt(ui_wifi, "Connecting to %s ...", g_try_ssid.c_str()); break;
    case WIFI_FAILED:     lv_label_set_text_fmt(ui_wifi, "Could not connect to %s", g_try_ssid.c_str()); break;
    case WIFI_UP:         lv_label_set_text_fmt(ui_wifi, LV_SYMBOL_WIFI "  %s  (%d dBm)", g_ssid.c_str(), (int)WiFi.RSSI()); break;
  }
  if (g_wifi_request < 0) lv_obj_set_style_text_color(ui_wifi, lv_color_hex(g_wifi == WIFI_FAILED ? C_WARN : C_INK), 0);
  if (g_wifi == WIFI_UP) lv_label_set_text_fmt(ui_addr, "http://%s/", WiFi.localIP().toString().c_str());
  else lv_label_set_text(ui_addr, "-");
  const bool wifi_on = g_wifi_request >= 0 ? g_wifi_request : g_wifi != WIFI_DISABLED;
  if (!wifi_on) lv_obj_remove_state(ui_wifi_sw, LV_STATE_CHECKED);
  else lv_obj_add_state(ui_wifi_sw, LV_STATE_CHECKED);
  lv_label_set_text(ui_pcstate, pc_state());
  lv_label_set_text(ui_pause_lbl, g_user_pause ? LV_SYMBOL_PLAY "  Resume" : LV_SYMBOL_PAUSE "  Pause");
  const bool armed = g_reset_armed && esp_timer_get_time() / 1000 - g_reset_armed < RESET_CONFIRM_MS;
  if (!armed) g_reset_armed = 0;
  lv_label_set_text(ui_reset_lbl, g_pc_stopped ? LV_SYMBOL_PLAY "  Start PC" : armed ? "Tap to confirm" : LV_SYMBOL_REFRESH "  Reset PC");
  if (g_pc_stopped) {   // Start PC alone, where Pause was
    lv_obj_add_flag(lv_obj_get_parent(ui_pause_lbl), LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(lv_obj_get_parent(ui_reset_lbl), 0, 36);
  }
  char batt[128];
  battery_text_short(batt, sizeof(batt));
  if (strcmp(lv_label_get_text(ui_batt), batt)) lv_label_set_text(ui_batt, batt);
  const bool low = g_batt.presence.state == BatteryPresence::Present && g_batt.level <= 15 && !g_batt.external;
  lv_obj_set_style_text_color(ui_batt, lv_color_hex(low ? 0x8a1208 : C_BLACK), 0);   // (on the title bar)
  {   // the machines: Start (two taps), unless the one chosen is running
    const int sel = lv_dropdown_get_selected(ui_mach_dd);
    const bool running_it = sel < g_machine_count && machine_path() == "/sdcard/" + String(g_machines[sel].file);
    const bool sarmed = g_start_armed && esp_timer_get_time() / 1000 - g_start_armed < RESET_CONFIRM_MS;
    if (!sarmed) g_start_armed = 0;
    lv_label_set_text(lv_obj_get_child(ui_mach_start, 0), running_it ? "Running" : sarmed ? "Confirm" : LV_SYMBOL_PLAY "  Start");
    if (running_it || !g_machine_count) lv_obj_add_state(ui_mach_start, LV_STATE_DISABLED);
    else lv_obj_remove_state(ui_mach_start, LV_STATE_DISABLED);
    // a new machine: DOS (fd_install.h), Linux (linux_install.h) or from a CD image on the card (fd_install.h),
    // installed after a restart with no PC
    const bool net = !g_ssid.isEmpty();
    // (no machine: an empty card, where the panel opens by itself, with Wi-Fi on)
    const char* msg =
        !net ? (g_machine_count ? "A new machine: DOS or Linux once Wi-Fi is set up (a network on the right); ISO from "
                                  "an installer CD's .iso at the card's root."
                                : "No machine on the card yet: choose a Wi-Fi network on the right, then install DOS or "
                                  "Linux; or ISO, from an installer CD's .iso at the card's root.")
        : g_machine_count ? "A new machine: DOS (FreeDOS 1.4, an 18 MB download), Linux (Sprig Linux, 10 MB, 140 MB on "
                            "the card) or ISO (an installer CD's .iso on the card). The Tab5 restarts to install it."
        : "No machine yet: DOS (FreeDOS 1.4, an 18 MB download), Linux (Sprig Linux, 10 MB, 140 MB on the card) or ISO "
          "(an installer CD's .iso on the card). The Tab5 restarts to install it.";
    if (strcmp(lv_label_get_text(ui_inst_msg), msg)) lv_label_set_text(ui_inst_msg, msg);
    for (lv_obj_t* b : {ui_dos_btn, ui_lnx_btn, ui_iso_btn}) {
      if (b == ui_iso_btn ? g_fd_iso_count > 0 : net) lv_obj_remove_state(b, LV_STATE_DISABLED);
      else lv_obj_add_state(b, LV_STATE_DISABLED);
    }
  }
}

// ---- network list -------------------------------------------------------------------------------------------

static const char* signal_word(int rssi) {
  return rssi >= -55 ? "excellent" : rssi >= -67 ? "good" : rssi >= -78 ? "fair" : "weak";
}

static void ui_show_modal(const char* ssid, bool secure);

static void net_clicked(lv_event_t* e) {
  const int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0) ui_show_modal(nullptr, true);   // "Other network..."
  else ui_show_modal(g_nets[i].ssid, g_nets[i].secure);
}

// A row of the network list: a selection bar when pressed, as in a setup program's menus
static void ui_list_row(lv_obj_t* b, uint32_t color) {
  lv_obj_set_height(b, 58);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_BAR), LV_STATE_PRESSED);
  lv_obj_set_style_text_color(b, lv_color_hex(color), 0);
  lv_obj_set_style_text_color(b, lv_color_hex(C_BLACK), LV_STATE_PRESSED);
  lv_obj_set_style_text_font(b, &px_font_24, 0);
  lv_obj_set_style_border_color(b, lv_color_hex(C_DEEP), 0);
  lv_obj_set_style_radius(b, 0, 0);
}

static void ui_fill_list() {
  lv_obj_clean(ui_list);
  if (g_wifi == WIFI_DISABLED) return;
  for (int i = 0; i < g_net_count; i++) {
    const Net& n = g_nets[i];
    const bool current = g_wifi == WIFI_UP && g_ssid == n.ssid;
    lv_obj_t* b = lv_list_add_button(ui_list, current ? LV_SYMBOL_OK : LV_SYMBOL_WIFI, n.ssid);
    ui_list_row(b, current ? C_ACCENT : C_DIM);
    lv_obj_t* name = lv_obj_get_child(b, 1);   // (the icon, the name: a long one ends in "...")
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(name, 1);
    lv_obj_t* info = ui_label(b, "", &px_font_24, C_INK);
    lv_label_set_text_fmt(info, "%s, %s", signal_word(n.rssi), n.secure ? "secured" : "open");
    lv_obj_add_event_cb(b, net_clicked, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }
  lv_obj_t* other = lv_list_add_button(ui_list, LV_SYMBOL_PLUS, "Other network ...");
  ui_list_row(other, C_INK);
  lv_obj_add_event_cb(other, net_clicked, LV_EVENT_CLICKED, (void*)(intptr_t)-1);
}

static void ui_start_scan() {
  g_scan_wanted = false;
  if (g_wifi == WIFI_DISABLED) {
    g_net_count = 0;
    ui_fill_list();
    lv_label_set_text(ui_scan_lbl, "Switch Wi-Fi on to see the networks");
    return;
  }
  if (g_scanning) return;
  if (g_wifi == WIFI_CONNECTING) {   // a scan during a connection attempt fails: after it
    g_scan_wanted = true;
    lv_label_set_text(ui_scan_lbl, "Connecting - the scan follows ...");
    return;
  }
  if (WiFi.scanNetworks(true) == WIFI_SCAN_FAILED) { lv_label_set_text(ui_scan_lbl, "Scan failed - tap Scan to retry"); return; }
  g_scanning = true;
  lv_label_set_text(ui_scan_lbl, "Scanning ...");
}

static void ui_scan_poll() {
  if (!g_scanning) return;
  const int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  g_scanning = false;
  if (n < 0) { lv_label_set_text(ui_scan_lbl, "Scan failed - tap Scan to retry"); return; }
  g_net_count = 0;
  for (int i = 0; i < n; i++) {             // de-duplicate by SSID (keep the strongest), skip hidden networks
    const String ss = WiFi.SSID(i);
    if (ss.isEmpty()) continue;
    int j = 0;
    while (j < g_net_count && strcmp(g_nets[j].ssid, ss.c_str()) != 0) j++;
    const int8_t rssi = (int8_t)WiFi.RSSI(i);
    if (j == g_net_count) {
      if (g_net_count == MAX_NETS) continue;
      strlcpy(g_nets[j].ssid, ss.c_str(), sizeof(g_nets[j].ssid));
      g_net_count++;
    } else if (rssi <= g_nets[j].rssi) {
      continue;
    }
    g_nets[j].rssi = rssi;
    g_nets[j].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  for (int a = 1; a < g_net_count; a++)     // strongest first
    for (int b = a; b > 0 && g_nets[b].rssi > g_nets[b - 1].rssi; b--) { Net x = g_nets[b]; g_nets[b] = g_nets[b - 1]; g_nets[b - 1] = x; }
  lv_label_set_text_fmt(ui_scan_lbl, "%d network%s found", g_net_count, g_net_count == 1 ? "" : "s");
  ui_fill_list();
}

// ---- password dialog ----------------------------------------------------------------------------------------

static void ui_hide_modal() {
  g_connect_pending = false;
  lv_obj_add_flag(ui_modal, LV_OBJ_FLAG_HIDDEN);
}

static void ui_name_submit();
static void ui_connect() {
  if (g_modal_mode != MODAL_WIFI) { ui_name_submit(); return; }
  const char* ssid = lv_textarea_get_text(ui_ta_ssid);
  if (!ssid[0]) { lv_label_set_text(ui_msg, "Enter the network name"); return; }
  g_pending_ssid = ssid;
  wifi_start(ssid, lv_textarea_get_text(ui_ta_pass));
  g_connect_pending = true;
  lv_label_set_text_fmt(ui_msg, "Connecting to %s ...", ssid);
  lv_obj_set_style_text_color(ui_msg, lv_color_hex(C_DIM), 0);
  lv_obj_remove_flag(ui_spinner, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_state(ui_connect_btn, LV_STATE_DISABLED);
}

static void ui_show_modal(const char* ssid, bool secure) {
  const bool other = ssid == nullptr;
  g_modal_mode = MODAL_WIFI;
  lv_textarea_set_placeholder_text(ui_ta_ssid, "Network name");
  lv_obj_remove_flag(ui_pass_row, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ui_iso_dd, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(lv_obj_get_child(ui_connect_btn, 0), "Connect");
  lv_textarea_set_text(ui_ta_ssid, other ? "" : ssid);
  lv_textarea_set_text(ui_ta_pass, "");
  lv_textarea_set_password_mode(ui_ta_pass, true);
  lv_label_set_text(ui_show_btn, "Show");
  lv_label_set_text(ui_modal_title, other ? "Join another network" : ssid);
  lv_label_set_text(ui_msg, secure ? (other ? "Enter the network name and password" : "Enter the password") : "Open network");
  lv_obj_set_style_text_color(ui_msg, lv_color_hex(C_DIM), 0);
  lv_obj_add_flag(ui_spinner, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_state(ui_connect_btn, LV_STATE_DISABLED);
  if (other) lv_obj_remove_flag(ui_ta_ssid, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(ui_ta_ssid, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(ui_modal, LV_OBJ_FLAG_HIDDEN);
  if (!secure && !other) { ui_connect(); return; }   // nothing to type
  lv_obj_t* target = other ? ui_ta_ssid : ui_ta_pass;
  lv_keyboard_set_textarea(ui_kb, target);
  lv_obj_add_state(target, LV_STATE_FOCUSED);
  lv_obj_remove_state(other ? ui_ta_pass : ui_ta_ssid, LV_STATE_FOCUSED);
}

// A machine's name from a CD image's file name: its letters, digits, - and _, lower case ("" if none)
static String ui_iso_machine_name(const String& iso) {
  String n;
  for (int i = 0; i < (int)iso.length() - 4 && n.length() < 24; ++i) {
    const char c = tolower((unsigned char)iso[i]);
    if (isalnum((unsigned char)c) || c == '-' || c == '_') n += c;
  }
  return machine_name_ok(n.c_str()) ? n : String("");
}

// The CD image chosen in the dialog: its name suggested for the machine, unless one was typed
static void ui_iso_chosen() {
  g_update_ok = "";   // (another CD: Install asks again)
  lv_label_set_text(lv_obj_get_child(ui_connect_btn, 0), "Install");
  const String typed = lv_textarea_get_text(ui_ta_ssid);
  const int sel = lv_dropdown_get_selected(ui_iso_dd);
  if (sel >= g_fd_iso_count || (typed.length() && typed != g_iso_name)) return;
  g_iso_name = ui_iso_machine_name(g_fd_isos[sel]);
  lv_textarea_set_text(ui_ta_ssid, g_iso_name.c_str());
}

// The dialog asking for a new machine's name (Install DOS / Install Linux), and for Install ISO which CD image
static void ui_show_name_modal(ModalMode mode) {
  g_modal_mode = mode;
  g_update_ok = "";
  lv_label_set_text(ui_modal_title, mode == MODAL_DOS     ? "Install DOS: machine name"
                                    : mode == MODAL_LINUX ? "Install Linux: machine name"
                                                          : "Install ISO: machine name");
  lv_textarea_set_placeholder_text(ui_ta_ssid, "e.g. dos, games, linux");
  lv_textarea_set_text(ui_ta_ssid, "");
  lv_obj_remove_flag(ui_ta_ssid, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ui_pass_row, LV_OBJ_FLAG_HIDDEN);
  if (mode == MODAL_ISO) {   // the CD images at the card's root (ui_fill_machines), the first chosen
    String opts;
    for (int i = 0; i < g_fd_iso_count; ++i) opts += (i ? "\n" : "") + g_fd_isos[i];
    lv_dropdown_set_options(ui_iso_dd, opts.c_str());
    lv_dropdown_set_selected(ui_iso_dd, 0);
    lv_obj_remove_flag(ui_iso_dd, LV_OBJ_FLAG_HIDDEN);
    g_iso_name = "";
  } else {
    lv_obj_add_flag(ui_iso_dd, LV_OBJ_FLAG_HIDDEN);
  }
  lv_label_set_text(ui_msg, "Letters, digits, - and _ only");
  lv_obj_set_style_text_color(ui_msg, lv_color_hex(C_DIM), 0);
  lv_obj_add_flag(ui_spinner, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(lv_obj_get_child(ui_connect_btn, 0), "Install");
  lv_obj_remove_state(ui_connect_btn, LV_STATE_DISABLED);
  lv_obj_remove_flag(ui_modal, LV_OBJ_FLAG_HIDDEN);
  lv_keyboard_set_textarea(ui_kb, ui_ta_ssid);
  lv_obj_add_state(ui_ta_ssid, LV_STATE_FOCUSED);
  lv_obj_remove_state(ui_ta_pass, LV_STATE_FOCUSED);
  if (mode == MODAL_ISO) ui_iso_chosen();
}

static void ui_name_submit() {
  const String name = lv_textarea_get_text(ui_ta_ssid);
  auto warn = [](const char* m) {
    lv_label_set_text(ui_msg, m);
    lv_obj_set_style_text_color(ui_msg, lv_color_hex(C_WARN), 0);
  };
  if (!machine_name_ok(name.c_str())) { warn("A name of letters, digits, - and _ (not \"default\")"); return; }
  const bool iso = g_modal_mode == MODAL_ISO;
  const int sel = iso ? lv_dropdown_get_selected(ui_iso_dd) : 0;
  if (iso && sel >= g_fd_iso_count) { warn("No CD image (.iso) at the card's root"); return; }
  if (!iso && g_ssid.isEmpty()) { warn("Set up Wi-Fi first (a network on the right)"); return; }
  const bool update = g_modal_mode == MODAL_LINUX && lx_installed(name.c_str());
  if (machine_name_used(name.c_str()) && !update) { warn("That name is in use (its .ini or folder is on the card)"); return; }
  if (!name.equalsIgnoreCase(g_update_ok)) {   // asked once: the PC stops (installs run with no PC: install_mode_run)
    g_update_ok = name;
    warn(update ? "It holds Sprig Linux: its kernel is updated, its files kept. The PC stops and the Tab5 restarts: again"
                : "The PC stops (anything unsaved is lost) and the Tab5 restarts to install: tap again");
    lv_label_set_text(lv_obj_get_child(ui_connect_btn, 0), "Restart");
    return;
  }
  if (iso) install_request('i', (name + ":" + g_fd_isos[sel]).c_str());
  else install_request(g_modal_mode == MODAL_DOS ? 'd' : 'l', name.c_str());
}

static void ui_connect_poll() {
  if (!g_connect_pending) return;
  if (g_attempt_failed) {
    g_connect_pending = false;
    lv_obj_add_flag(ui_spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(ui_connect_btn, LV_STATE_DISABLED);
    lv_label_set_text(ui_msg, "Could not connect. Check the password and try again.");
    lv_obj_set_style_text_color(ui_msg, lv_color_hex(C_WARN), 0);
  } else if (g_wifi == WIFI_UP && g_ssid == g_pending_ssid) {
    ui_hide_modal();
    ui_fill_list();
  }
}

static lv_obj_t* ui_textarea(lv_obj_t* parent, const char* placeholder) {
  lv_obj_t* ta = lv_textarea_create(parent);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_placeholder_text(ta, placeholder);
  lv_obj_set_width(ta, lv_pct(100));
  lv_obj_set_style_text_font(ta, &px_font_32, 0);
  lv_obj_set_style_bg_color(ta, lv_color_hex(C_DEEP), 0);
  lv_obj_set_style_text_color(ta, lv_color_hex(C_INK), 0);
  lv_obj_set_style_border_color(ta, lv_color_hex(C_LINE), 0);
  lv_obj_set_style_border_width(ta, 2, 0);
  lv_obj_set_style_radius(ta, 0, 0);
  lv_obj_set_style_border_color(ta, lv_color_hex(C_ACCENT), LV_STATE_FOCUSED);
  lv_obj_set_style_text_color(ta, lv_color_hex(C_DIM), LV_PART_TEXTAREA_PLACEHOLDER);
  lv_obj_set_style_bg_color(ta, lv_color_hex(C_ACCENT), LV_PART_CURSOR);   // (a block cursor's colour)
  lv_obj_add_event_cb(ta, [](lv_event_t* e) {   // the keyboard types into whichever field was tapped
    lv_obj_t* t = (lv_obj_t*)lv_event_get_target(e);
    lv_keyboard_set_textarea(ui_kb, t);
    lv_obj_remove_state(t == ui_ta_ssid ? ui_ta_pass : ui_ta_ssid, LV_STATE_FOCUSED);
  }, LV_EVENT_FOCUSED, nullptr);
  return ta;
}

// LVGL's stock layout has ". , :" after M; use the usual ", ." order (the rest is LVGL's default).
#define KB_KEY(w) ((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_POPOVER | (w)))     // letter key, width w
#define KB_MODE(w) ((lv_buttonmatrix_ctrl_t)(LV_KEYBOARD_CTRL_BUTTON_FLAGS | (w)))   // mode switch / close / OK
#define KB_FN(w) ((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_CHECKED | (w)))      // backspace, enter, arrows
static const char* const kb_lower[] = {
  "1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
  "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
  "_", "-", "z", "x", "c", "v", "b", "n", "m", ",", ".", ":", "\n",
  LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""};
static const char* const kb_upper[] = {
  "1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
  "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
  "_", "-", "Z", "X", "C", "V", "B", "N", "M", ",", ".", ":", "\n",
  LV_SYMBOL_CLOSE, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""};
static const lv_buttonmatrix_ctrl_t kb_ctrl[] = {
  KB_MODE(5), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_KEY(4),
  KB_KEY(4), KB_KEY(4), KB_KEY(4), KB_FN(7),
  KB_MODE(6), KB_KEY(3), KB_KEY(3), KB_KEY(3), KB_KEY(3), KB_KEY(3), KB_KEY(3), KB_KEY(3),
  KB_KEY(3), KB_KEY(3), KB_FN(7),
  KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1), KB_KEY(1),
  KB_KEY(1),
  KB_MODE(2), KB_FN(2), (lv_buttonmatrix_ctrl_t)6, KB_FN(2),
  KB_MODE(2)};

// A list to choose from, in the panel's colours
static void ui_style_dropdown(lv_obj_t* dd) {
  lv_obj_set_style_text_font(dd, &px_font_24, 0);
  lv_obj_set_style_bg_color(dd, lv_color_hex(C_DEEP), 0);
  lv_obj_set_style_text_color(dd, lv_color_hex(C_ACCENT), 0);
  lv_obj_set_style_border_color(dd, lv_color_hex(C_LINE), 0);
  lv_obj_set_style_radius(dd, 0, 0);
  lv_obj_t* l = lv_dropdown_get_list(dd);
  lv_obj_set_style_text_font(l, &px_font_24, 0);
  lv_obj_set_style_bg_color(l, lv_color_hex(C_DEEP), 0);
  lv_obj_set_style_text_color(l, lv_color_hex(C_INK), 0);
  lv_obj_set_style_radius(l, 0, 0);
  lv_obj_set_style_bg_color(l, lv_color_hex(C_BAR), LV_PART_SELECTED | LV_STATE_CHECKED);
  lv_obj_set_style_text_color(l, lv_color_hex(C_BLACK), LV_PART_SELECTED | LV_STATE_CHECKED);
}

static void ui_build_modal(lv_obj_t* scr) {
  ui_modal = lv_obj_create(scr);
  lv_obj_set_size(ui_modal, SCR_W, SCR_H);
  lv_obj_set_pos(ui_modal, 0, 0);
  lv_obj_set_style_bg_color(ui_modal, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(ui_modal, LV_OPA_70, 0);
  lv_obj_set_style_border_width(ui_modal, 0, 0);
  lv_obj_set_style_radius(ui_modal, 0, 0);
  lv_obj_remove_flag(ui_modal, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* box = ui_card(ui_modal, 240, 14, 800, 0);   // as tall as what it shows (at most ~358: the keyboard's
  lv_obj_set_height(box, LV_SIZE_CONTENT);              // top is at y 400, and the box ~12 lower: ui_modal's padding)
  lv_obj_set_style_pad_all(box, 18, 0);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(box, 12, 0);
  lv_obj_set_style_shadow_width(box, 1, 0);   // a hard shadow, as a dialog box had
  lv_obj_set_style_shadow_offset_x(box, 16, 0);
  lv_obj_set_style_shadow_offset_y(box, 16, 0);
  lv_obj_set_style_shadow_color(box, lv_color_hex(0x000000), 0);
  lv_obj_set_style_shadow_opa(box, LV_OPA_60, 0);
  ui_modal_title = ui_label(box, "", &px_font_32, C_ACCENT);
  ui_ta_ssid = ui_textarea(box, "Network name");
  lv_obj_t* row = ui_pass_row = lv_obj_create(box);   // password field + Show
  lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 12, 0);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  ui_ta_pass = ui_textarea(row, "Password");
  lv_obj_set_flex_grow(ui_ta_pass, 1);
  lv_textarea_set_password_mode(ui_ta_pass, true);
  lv_obj_t* show = ui_button(row, "Show", [](lv_event_t*) {
    const bool hidden = lv_textarea_get_password_mode(ui_ta_pass);
    lv_textarea_set_password_mode(ui_ta_pass, !hidden);
    lv_label_set_text(ui_show_btn, hidden ? "Hide" : "Show");
  }, false);
  lv_obj_set_size(show, 110, 58);
  ui_show_btn = lv_obj_get_child(show, 0);
  ui_iso_dd = lv_dropdown_create(box);   // Install ISO's CD image, where the password goes
  lv_obj_set_size(ui_iso_dd, lv_pct(100), 58);
  ui_style_dropdown(ui_iso_dd);
  lv_obj_add_event_cb(ui_iso_dd, [](lv_event_t*) { ui_iso_chosen(); }, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_flag(ui_iso_dd, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* msg_row = lv_obj_create(box);
  lv_obj_set_size(msg_row, lv_pct(100), 34);
  lv_obj_set_style_bg_opa(msg_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(msg_row, 0, 0);
  lv_obj_set_style_pad_all(msg_row, 0, 0);
  lv_obj_remove_flag(msg_row, LV_OBJ_FLAG_SCROLLABLE);
  ui_spinner = lv_spinner_create(msg_row);
  lv_obj_set_size(ui_spinner, 32, 32);
  lv_obj_align(ui_spinner, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_arc_color(ui_spinner, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(ui_spinner, lv_color_hex(C_DEEP), 0);
  lv_obj_set_style_arc_rounded(ui_spinner, false, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(ui_spinner, 5, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(ui_spinner, 5, 0);
  ui_msg = ui_label(msg_row, "", &px_font_24, C_DIM);
  lv_obj_align(ui_msg, LV_ALIGN_LEFT_MID, 44, 0);

  lv_obj_t* buttons = lv_obj_create(box);
  lv_obj_set_size(buttons, lv_pct(100), 64);
  lv_obj_set_style_bg_opa(buttons, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(buttons, 0, 0);
  lv_obj_set_style_pad_all(buttons, 0, 0);
  lv_obj_remove_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* cancel = ui_button(buttons, "Cancel", [](lv_event_t*) { ui_hide_modal(); }, false);
  lv_obj_set_size(cancel, 200, 64);
  lv_obj_align(cancel, LV_ALIGN_LEFT_MID, 0, 0);
  ui_connect_btn = ui_button(buttons, "Connect", [](lv_event_t*) { ui_connect(); }, true);
  lv_obj_set_size(ui_connect_btn, 240, 64);
  lv_obj_align(ui_connect_btn, LV_ALIGN_RIGHT_MID, 0, 0);

  ui_kb = lv_keyboard_create(ui_modal);
  lv_obj_set_size(ui_kb, SCR_W, 300);
  lv_obj_align(ui_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_text_font(ui_kb, &px_font_32, 0);   // keycaps: grey, the function keys cyan, yellow when pressed
  lv_obj_set_style_bg_color(ui_kb, lv_color_hex(C_DEEP), 0);
  lv_obj_set_style_radius(ui_kb, 0, 0);
  lv_obj_set_style_radius(ui_kb, 0, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(ui_kb, lv_color_hex(C_KEY), LV_PART_ITEMS);
  lv_obj_set_style_text_color(ui_kb, lv_color_hex(C_BLACK), LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(ui_kb, 0, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(ui_kb, lv_color_hex(C_BAR), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_color(ui_kb, lv_color_hex(C_BLACK), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_bg_color(ui_kb, lv_color_hex(C_ACCENT), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_keyboard_set_map(ui_kb, LV_KEYBOARD_MODE_TEXT_LOWER, kb_lower, kb_ctrl);
  lv_keyboard_set_map(ui_kb, LV_KEYBOARD_MODE_TEXT_UPPER, kb_upper, kb_ctrl);
  lv_obj_add_event_cb(ui_kb, [](lv_event_t*) { ui_connect(); }, LV_EVENT_READY, nullptr);       // the keyboard's OK key
  lv_obj_add_event_cb(ui_kb, [](lv_event_t*) { ui_hide_modal(); }, LV_EVENT_CANCEL, nullptr);   // its close key
  lv_obj_add_flag(ui_modal, LV_OBJ_FLAG_HIDDEN);
}

// ---- the Tab5 Keyboard while the panel is open --------------------------------------------------------------

// The character a key types (US layout), or 0.
static char kbd_char(KbdKey k) {
  static const char lower[] = "abcdefghijklmnopqrstuvwxyz1234567890\n\x1b\b\t -=[]\\#;'`,./";
  static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x1b\b\t _+{}|~:\"~<>?";
  if (k.code < 0x04 || k.code > 0x38) return 0;
  return (k.mod & KMOD_SHIFT ? upper : lower)[k.code - 0x04];
}

static void kbd_type_in_panel(KbdKey k) {
  if (lv_obj_has_flag(ui_modal, LV_OBJ_FLAG_HIDDEN)) {   // the panel itself: Esc closes it
    if (k.code == 0x29) panel_close();
    return;
  }
  lv_obj_t* ta = lv_keyboard_get_textarea(ui_kb);
  const char c = kbd_char(k);
  if (c == '\n') ui_connect();
  else if (c == '\x1b') ui_hide_modal();
  else if (c == '\b') { if (ta) lv_textarea_delete_char(ta); }
  else if (k.code == 0x4C) { if (ta) lv_textarea_delete_char_forward(ta); }
  else if (k.code == 0x50) { if (ta) lv_textarea_cursor_left(ta); }
  else if (k.code == 0x4F) { if (ta) lv_textarea_cursor_right(ta); }
  else if (k.code == 0x4A) { if (ta) lv_textarea_set_cursor_pos(ta, 0); }
  else if (k.code == 0x4D) { if (ta) lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST); }
  else if (c == '\t' && g_modal_mode == MODAL_WIFI) {   // to the other field, if it is shown
    lv_obj_t* other = ta == ui_ta_ssid ? ui_ta_pass : ui_ta_ssid;
    if (!lv_obj_has_flag(other, LV_OBJ_FLAG_HIDDEN)) {
      lv_keyboard_set_textarea(ui_kb, other);
      lv_obj_add_state(other, LV_STATE_FOCUSED);
      if (ta) lv_obj_remove_state(ta, LV_STATE_FOCUSED);
    }
  } else if (c && ta) {
    lv_textarea_add_char(ta, c);
  }
}

// ---- panel --------------------------------------------------------------------------------------------------

static bool g_redraw_pc = false;   // the PC's screen has to be drawn in full again (after the panel)
static bool g_side_on = true;      // the buttons and lights beside the PC's picture (side_ui.h; NVS "side")

static void panel_close() {
  if (!g_panel_open) return;
  ui_wifi_request_do();   // (a switch tapped just before)
  ui_hide_modal();
  g_panel_open = false;
  if (WiFi.scanComplete() >= 0) WiFi.scanDelete();
  g_scanning = false;
  g_redraw_pc = true;
  Serial.println("settings closed");
}

static void panel_open() {
  if (g_panel_open) return;
  g_panel_open = true;
  g_touch_block = M5.Touch.getCount() > 0;
  side_cancel();       // a side button held (the panel opened by a key): its release is the panel's now
  kbd_release_all();   // keys held for the PC
  usb_kbd_release_all();
  lv_slider_set_value(ui_bri, g_brightness, LV_ANIM_OFF);
  lv_slider_set_value(ui_vol, g_volume, LV_ANIM_OFF);
  lv_slider_set_value(ui_mouse, (int)(g_mouse_gain * 10), LV_ANIM_OFF);
  if (g_side_on) lv_obj_add_state(ui_side_sw, LV_STATE_CHECKED);
  else lv_obj_remove_state(ui_side_sw, LV_STATE_CHECKED);
  g_reset_armed = 0;
  ui_fill_machines();
  ui_refresh_status();
  ui_fill_list();
  lv_obj_invalidate(lv_screen_active());
  ui_start_scan();
  Serial.println("settings open");
}

static void panel_toggle() {
  if (g_panel_open) panel_close();
  else panel_open();
}

// From the core-0 task while the panel is open.
static void ui_poll() {
  static int64_t next = 0;
  const int64_t now = esp_timer_get_time() / 1000;
  ui_scan_poll();
  ui_connect_poll();
  if (now >= next) { ui_refresh_status(); next = now + 500; }
  lv_timer_handler();
  if (g_scan_wanted && g_wifi != WIFI_CONNECTING) ui_start_scan();
  if (g_wifi_request >= 0 && now - g_wifi_request_at >= WIFI_REQUEST_MS) {   // the panel shows the switch as tapped
    lv_refr_now(g_disp);                                                    // (and "Switching ..."), then the work
    ui_wifi_request_do();
    ui_refresh_status();
    ui_start_scan();
  }
}

static void ui_wifi_request_do() {
  if (g_wifi_request < 0) return;
  const bool on = g_wifi_request;
  g_wifi_request = -1;
  wifi_enable(on);
}

static void ui_setup() {
  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return (uint32_t)(esp_timer_get_time() / 1000); });
  g_disp = lv_display_create(SCR_W, SCR_H);
  lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(g_disp, ui_flush);
  const size_t buf = SCR_W * 80 * 2;   // (one: ui_flush is done when it returns, so a second would never be drawn in
                                       // while the first is sent)
  lv_display_set_buffers(g_disp, ui_alloc(buf), nullptr, buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_theme(g_disp, lv_theme_default_init(g_disp, lv_color_hex(C_ACCENT), lv_color_hex(C_DIM), true,
                                                      &px_font_24));
  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, ui_touch);

  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  // the title bar: name and version, and the battery on the right, like a setup program's
  lv_obj_t* top = lv_obj_create(scr);
  lv_obj_set_pos(top, 0, 0);
  lv_obj_set_size(top, SCR_W, 56);
  lv_obj_set_style_bg_color(top, lv_color_hex(C_BAR), 0);
  lv_obj_set_style_border_width(top, 0, 0);
  lv_obj_set_style_radius(top, 0, 0);
  lv_obj_set_style_pad_all(top, 0, 0);
  lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* title = ui_label(top, "TAB5 PC SETUP", &px_font_32, C_BLACK);
  lv_obj_set_pos(title, 30, 12);
  lv_obj_t* sub = ui_label(top, "", &px_font_24, C_DEEP);
  lv_label_set_text_fmt(sub, "v%s", FIRMWARE_VERSION);
  lv_obj_set_pos(sub, 380, 18);
  ui_batt = ui_label(top, "", &px_font_24, C_BLACK);
  lv_obj_set_width(ui_batt, 700);
  lv_obj_set_style_text_align(ui_batt, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_pos(ui_batt, SCR_W - 30 - 700, 18);
  // the keys, at the bottom like a setup program's help line
  lv_obj_t* help = lv_obj_create(scr);
  lv_obj_set_pos(help, 0, SCR_H - 38);
  lv_obj_set_size(help, SCR_W, 38);
  lv_obj_set_style_bg_color(help, lv_color_hex(C_BAR), 0);
  lv_obj_set_style_border_width(help, 0, 0);
  lv_obj_set_style_radius(help, 0, 0);
  lv_obj_set_style_pad_all(help, 0, 0);
  lv_obj_remove_flag(help, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* keys = ui_label(help, "This panel: swipe down, Sym+Esc, Scroll Lock     Volume: Sym+Alt+Up/Down",
                            &px_font_24, C_BLACK);
  lv_obj_set_pos(keys, 30, 7);

  constexpr int LX = 30, LW = 540, RX = 600, RW = 650, TOP = 86;
  // the PC, and where to copy files to it
  lv_obj_t* st = ui_card(scr, LX, TOP, LW, 272);
  ui_label(st, "State", &px_font_24, C_DIM);
  ui_pcstate = ui_label(st, "", &px_font_24, C_ACCENT);
  lv_obj_set_pos(ui_pcstate, 110, 0);
  lv_obj_t* pause = ui_button(st, "", [](lv_event_t*) { pc_set_paused(!g_user_pause); ui_refresh_status(); }, false);
  lv_obj_set_size(pause, 200, 52);
  lv_obj_set_pos(pause, 0, 36);
  ui_pause_lbl = lv_obj_get_child(pause, 0);
  lv_obj_t* reset = ui_button(st, "", [](lv_event_t*) {   // two taps: a stray one can't lose the user's work
    if (g_pc_stopped) restart_tab5();   // (stopped from the web page: Start PC)
    const int64_t now = esp_timer_get_time() / 1000;
    if (g_reset_armed && now - g_reset_armed < RESET_CONFIRM_MS) {
      g_reset_armed = 0;
      pc_cold_reset();
      pc_set_paused(false);
      panel_close();
    } else {
      g_reset_armed = now;
    }
    if (g_panel_open) ui_refresh_status();
  }, false);
  lv_obj_set_size(reset, 280, 52);
  lv_obj_set_pos(reset, 210, 36);
  ui_reset_lbl = lv_obj_get_child(reset, 0);
  lv_obj_t* h2 = ui_label(st, "Copy files over Wi-Fi", &px_font_24, C_DIM);
  lv_obj_set_pos(h2, 0, 112);
  ui_wifi_sw = lv_switch_create(st);   // WiFi only runs for copying files
  ui_switch_style(ui_wifi_sw);
  lv_obj_set_pos(ui_wifi_sw, LW - 50 - 80, 104);   // (inside: the frame's 3 and the 22 each side)
  lv_obj_add_event_cb(ui_wifi_sw, [](lv_event_t*) {
    g_wifi_request = lv_obj_has_state(ui_wifi_sw, LV_STATE_CHECKED);   // (done by ui_poll, once shown)
    g_wifi_request_at = esp_timer_get_time() / 1000;
    ui_refresh_status();
  }, LV_EVENT_VALUE_CHANGED, nullptr);
  ui_switch_label(h2, ui_wifi_sw);
  ui_addr = ui_label(st, "", &px_font_24, C_ACCENT);
  lv_obj_set_pos(ui_addr, 0, 150);
  ui_wifi = ui_label(st, "", &px_font_24, C_INK);
  lv_label_set_long_mode(ui_wifi, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(ui_wifi, lv_pct(100));
  lv_obj_set_pos(ui_wifi, 0, 180);
  ui_caption(scr, "PC", LX, TOP, LW);

  // screen, sound, touchpad
  constexpr int DY = TOP + 272 + 24;
  lv_obj_t* dc = ui_card(scr, LX, DY, LW, 222);
  ui_bri = ui_slider(dc, "Brightness", 0, 8, 255, [](lv_event_t*) { set_brightness(lv_slider_get_value(ui_bri)); });
  ui_vol = ui_slider(dc, "Volume", 44, 0, 255, [](lv_event_t*) { set_volume(lv_slider_get_value(ui_vol)); });
  ui_mouse = ui_slider(dc, "Touchpad", 88, 3, 40, [](lv_event_t*) {
    g_mouse_gain = lv_slider_get_value(ui_mouse) / 10.0f;
    g_prefs.putFloat("mouse", g_mouse_gain);
  });
  lv_obj_t* side = ui_label(dc, "Side panels", &px_font_24, C_INK);
  lv_obj_set_pos(side, 0, 138);
  ui_side_sw = lv_switch_create(dc);   // off: the margins black, touches there the touchpad's
  ui_switch_style(ui_side_sw);
  lv_obj_set_pos(ui_side_sw, LW - 50 - 80, 130);
  lv_obj_add_event_cb(ui_side_sw, [](lv_event_t*) {
    g_side_on = lv_obj_has_state(ui_side_sw, LV_STATE_CHECKED);
    g_prefs.putBool("side", g_side_on);   // (drawn so when the panel closes: g_redraw_pc)
  }, LV_EVENT_VALUE_CHANGED, nullptr);
  ui_switch_label(side, ui_side_sw);
  ui_caption(scr, "Screen, sound, touchpad", LX, DY, LW);

  lv_obj_t* done = ui_button(scr, "Done", [](lv_event_t*) { panel_close(); }, true);
  lv_obj_set_pos(done, LX - 6, DY + 222 + 18);
  lv_obj_set_size(done, LW + 12, 46);
  lv_obj_set_style_text_font(lv_obj_get_child(done, 0), &px_font_32, 0);

  // networks
  constexpr int NH = 272;   // (as tall as the PC's frame: the machines below, as tall as the sliders')
  lv_obj_t* nc = ui_card(scr, RX, TOP, RW, NH);
  ui_scan_lbl = ui_label(nc, "", &px_font_24, C_DIM);
  lv_obj_set_pos(ui_scan_lbl, 0, 4);
  lv_obj_set_width(ui_scan_lbl, RW - 50 - 150 - 16);   // (left of Scan, in two lines if need be)
  lv_label_set_long_mode(ui_scan_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_t* scan = ui_button(nc, LV_SYMBOL_REFRESH "  Scan", [](lv_event_t*) { ui_start_scan(); }, false);
  lv_obj_set_size(scan, 150, 48);
  lv_obj_align(scan, LV_ALIGN_TOP_RIGHT, 0, 0);
  ui_list = lv_list_create(nc);
  lv_obj_set_pos(ui_list, 0, 64);
  lv_obj_set_size(ui_list, RW - 50, NH - 50 - 64);
  lv_obj_set_style_bg_color(ui_list, lv_color_hex(C_CARD), 0);
  lv_obj_set_style_border_width(ui_list, 0, 0);
  lv_obj_set_style_radius(ui_list, 0, 0);
  lv_obj_set_style_pad_all(ui_list, 0, 0);
  lv_obj_set_style_text_font(ui_list, &px_font_24, 0);
  ui_caption(scr, "Wi-Fi networks", RX, TOP, RW);

  // the machines (the card's settings files) and Sprig Linux (linux_install.h)
  lv_obj_t* mc = ui_card(scr, RX, DY, RW, 222);
  ui_mach_dd = lv_dropdown_create(mc);
  lv_obj_set_pos(ui_mach_dd, 0, 0);
  lv_obj_set_size(ui_mach_dd, 400, 48);
  ui_style_dropdown(ui_mach_dd);
  lv_obj_add_event_cb(ui_mach_dd, [](lv_event_t*) { g_start_armed = 0; ui_refresh_status(); }, LV_EVENT_VALUE_CHANGED,
                      nullptr);
  ui_mach_start = ui_button(mc, "", [](lv_event_t*) {   // two taps: the Tab5 restarts, the PC's work is lost
    const int sel = lv_dropdown_get_selected(ui_mach_dd);
    const int64_t now = esp_timer_get_time() / 1000;
    if (sel < g_machine_count && g_start_armed && now - g_start_armed < RESET_CONFIRM_MS) {
      machine_choose(g_machines[sel].file);
      restart_tab5();
    }
    g_start_armed = now;
    ui_refresh_status();
  }, false);
  lv_obj_set_size(ui_mach_start, RW - 50 - 412, 48);
  lv_obj_set_pos(ui_mach_start, 412, 0);
  ui_inst_msg = ui_label(mc, "", &lv_font_montserrat_20, C_INK);   // (prose: the smaller face)
  lv_label_set_long_mode(ui_inst_msg, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(ui_inst_msg, lv_pct(100));
  lv_obj_set_pos(ui_inst_msg, 0, 58);
  // Install: DOS, Linux, ISO (three to a row: "Install" is in the dialog's title)
  constexpr int BW = (RW - 50 - 2 * 12) / 3;
  ui_dos_btn = ui_button(mc, LV_SYMBOL_DOWNLOAD "  DOS", [](lv_event_t*) { ui_show_name_modal(MODAL_DOS); }, true);
  ui_lnx_btn = ui_button(mc, LV_SYMBOL_DOWNLOAD "  Linux", [](lv_event_t*) { ui_show_name_modal(MODAL_LINUX); }, true);
  ui_iso_btn = ui_button(mc, LV_SYMBOL_DRIVE "  ISO", [](lv_event_t*) { ui_show_name_modal(MODAL_ISO); }, true);
  int bx = 0;
  for (lv_obj_t* b : {ui_dos_btn, ui_lnx_btn, ui_iso_btn}) {
    lv_obj_set_size(b, BW, 46);
    lv_obj_set_pos(b, bx, 126);
    bx += BW + 12;
  }
  ui_caption(scr, "Machine", RX, DY, RW);

  ui_build_modal(scr);
}
