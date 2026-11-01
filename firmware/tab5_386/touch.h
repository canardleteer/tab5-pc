// The touch screen as a touchpad for the PC's PS/2 mouse (DOS programs need a driver: CTMOUSE, loaded by
// FDAUTO.BAT on the prepared C: disk).
//   one finger moving       the pointer moves (relative, like a laptop touchpad; serial "mouse <gain>")
//   tap                     left click
//   two-finger tap          right click
//   tap, then touch again   (within TAP_GAP_MS) left button held while that finger moves: drag
//   swipe down from the top edge   the settings panel (touches that start in the top EDGE_PX don't move the pointer)
// While the panel is open, touches go to it (settings_ui.h reads M5.Touch after touch_poll() has updated it).
// The touch controller is on M5Unified's internal I2C bus, which the speaker's codec shares: g_i2c_lock.
#pragma once

static constexpr int64_t TOUCH_POLL_MS = 10;
static constexpr int64_t TAP_MS = 220;        // a touch shorter than this, that hardly moved, is a tap
static constexpr int TAP_MOVE = 12;           // "hardly": screen pixels
static constexpr int64_t TAP_GAP_MS = 250;    // a touch this soon after a tap drags
static constexpr int64_t CLICK_MS = 40;       // how long a tap holds the button
static constexpr int EDGE_PX = 40, SWIPE_PX = 120;   // the top edge, and how far down a swipe from it goes

static float g_mouse_gain = 1.0f;             // PS/2 counts per screen pixel of finger movement
static bool side_owns(int x, int y);           // (side_ui.h)
static void side_touch(int x, int y, int kind, int64_t now);

struct Touchpad {
  bool down = false, dragging = false, side = false;   // side: a touch the margins' buttons have (side_ui.h)
  int fingers = 0;                            // most fingers seen during this touch
  int x = 0, y = 0, x0 = 0, y0 = 0, travel = 0;
  int64_t t_down = 0, t_last_tap = -10000, release_at = 0;
  float fx = 0, fy = 0;                       // movement not yet sent (fractions of a count)
  int buttons = 0;
  uint32_t taps = 0, right_taps = 0;
};
static Touchpad g_tp;

static void mouse_send(int dx, int dy, int buttons) {
  if (g_pc) input_put({1, (int8_t)buttons, (int16_t)dx, (int16_t)dy});
  g_tp.buttons = buttons;
}

static void touch_poll() {
  static int64_t next = 0;
  const int64_t now = esp_timer_get_time() / 1000;
  if (now < next) return;
  next = now + TOUCH_POLL_MS;
  Touchpad& t = g_tp;
  if (t.release_at && now >= t.release_at) {   // the end of a tap's click
    t.release_at = 0;
    mouse_send(0, 0, 0);
  }
  xSemaphoreTake(g_i2c_lock, portMAX_DELAY);
  M5.update();
  xSemaphoreGive(g_i2c_lock);
  const int n = M5.Touch.getCount();
  if (g_panel_open) {   // the panel has the touches
    t.down = n > 0;
    t.dragging = false;
    return;
  }
  if (n > 0) {
    const auto& d = M5.Touch.getDetail(0);
    if (!t.down && side_owns(d.x, d.y)) {   // beside the PC's picture: its buttons
      t.down = t.side = true;
      t.x = d.x;
      t.y = d.y;
      side_touch(d.x, d.y, 1, now);
      return;
    }
    if (t.side) {
      t.x = d.x;
      t.y = d.y;
      side_touch(d.x, d.y, 2, now);
      return;
    }
    if (!t.down) {   // finger down
      t.down = true;
      t.fingers = n;
      t.x = t.x0 = d.x;
      t.y = t.y0 = d.y;
      t.travel = 0;
      t.fx = t.fy = 0;
      t.t_down = now;
      t.dragging = now - t.t_last_tap < TAP_GAP_MS;
      if (t.dragging) {
        t.release_at = 0;
        mouse_send(0, 0, 1);
      }
      return;
    }
    t.fingers = max(t.fingers, n);
    const int dx = d.x - t.x, dy = d.y - t.y;
    t.x = d.x;
    t.y = d.y;
    t.travel = max(t.travel, abs(d.x - t.x0) + abs(d.y - t.y0));
    if (t.y0 < EDGE_PX && !t.dragging) {   // from the top edge: a swipe down opens the panel; no pointer movement
      if (d.y - t.y0 > SWIPE_PX) {
        t.down = false;
        panel_toggle();
      }
      return;
    }
    if (t.fingers > 1 || (!dx && !dy)) return;   // two fingers: a right tap, not movement
    t.fx += dx * g_mouse_gain;
    t.fy += dy * g_mouse_gain;
    const int mx = (int)t.fx, my = (int)t.fy;
    if (mx || my) {
      t.fx -= mx;
      t.fy -= my;
      mouse_send(mx, my, t.dragging ? 1 : t.buttons);
    }
    return;
  }
  if (!t.down) return;
  t.down = false;   // finger up
  if (t.side) {
    t.side = false;
    side_touch(t.x, t.y, 0, now);
    return;
  }
  const bool tap = now - t.t_down < TAP_MS && t.travel < TAP_MOVE && t.y0 >= EDGE_PX;
  if (t.dragging) {
    t.dragging = false;
    mouse_send(0, 0, 0);
  } else if (tap) {
    const int button = t.fingers > 1 ? 2 : 1;   // PS/2 buttons: 1 left, 2 right
    mouse_send(0, 0, button);
    t.release_at = now + CLICK_MS;
    if (button == 1) {
      t.t_last_tap = now;
      ++t.taps;
    } else {
      ++t.right_taps;
    }
  }
}
