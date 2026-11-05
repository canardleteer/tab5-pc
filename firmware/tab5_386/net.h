// WiFi, only for copying files: off from startup, switched on in the settings panel (or serial "wifi on"); then it
// joins the saved network (Preferences "ssid"/"pass") and starts the web server (web.h) once it is up. Off
// again, the radio stops and the server with it.
// Joining is non-blocking: wifi_poll() follows it from the core-0 task.
#pragma once
#include <WiFi.h>
#include <esp_wifi.h>

enum WifiState { WIFI_DISABLED, WIFI_NONE, WIFI_CONNECTING, WIFI_UP, WIFI_FAILED };
static WifiState g_wifi = WIFI_DISABLED;
static void clock_ntp(bool on);   // (clock.h: SNTP while Wi-Fi is up)
static bool g_wifi_ready = false;           // the WiFi stack (ESP-Hosted, to the C6) has been brought up
static String g_ssid, g_pass;               // the saved network
static String g_try_ssid, g_try_pass;       // the network being joined; saved once that works
static int64_t g_wifi_t0 = 0;
static bool g_attempt_failed = false;       // the last wifi_start() didn't work (for the settings panel)
static constexpr int64_t CONNECT_TIMEOUT_MS = 20000, RETRY_MS = 30000;

static void web_start();
static void web_stop();
static void guest_net_start();     // guest_net.h
static void guest_net_wifi_up();
static void pc_hold(bool on);   // the .ino

static void wifi_load() {
  g_ssid = g_prefs.getString("ssid", "");
  g_pass = g_prefs.getString("pass", "");
}

// Start joining a network (wifi_poll() follows it up). The credentials are saved once it works.
static void wifi_enable(bool on);
static void wifi_start(const String& ssid, const String& pass) {
  if (g_wifi == WIFI_DISABLED) {
    g_ssid = ssid;   // switching on joins it
    g_pass = pass;
    wifi_enable(true);
    return;
  }
  g_try_ssid = ssid;
  g_try_pass = pass;
  g_attempt_failed = false;
  WiFi.disconnect(false, false);   // keep the WiFi stack up (this firmware never takes it down)
  WiFi.begin(ssid.c_str(), pass.c_str());
  g_wifi = WIFI_CONNECTING;
  g_wifi_t0 = esp_timer_get_time() / 1000;
  Serial.printf("wifi: connecting to %s\n", ssid.c_str());
}

static void wifi_setup() { wifi_load(); }

// Switch WiFi on (joining the saved network, if any) or off. The stack, once up, stays up (this firmware doesn't
// take ESP-Hosted down and up again): off stops the radio only.
static void wifi_enable(bool on) {
  if (on == (g_wifi != WIFI_DISABLED)) return;
  if (on) {
    if (!g_wifi_ready) {
      // WiFi takes ~160 KB of internal RAM (ESP-Hosted's DMA buffers, lwIP's packets): with the rest of the firmware,
      // all there is, and an allocation that failed reset the Tab5 or broke the SD card's reads. From here on,
      // malloc() puts blocks of 512 bytes and more in PSRAM (not only those of 4 KB and more): lwIP's packets go
      // there. (Memory for DMA is asked for as such, and stays internal.)
      heap_caps_malloc_extmem_enable(512);
      // the C6 shares the SD card's host: no disk access by the PC while its link starts
      pc_hold(true);
      WiFi.mode(WIFI_STA);
      WiFi.setAutoReconnect(true);
      pc_hold(false);
      g_wifi_ready = true;
      guest_net_start();   // (done at the start; again only if that failed)
    } else {
      esp_wifi_start();
    }
    g_wifi = WIFI_NONE;
    Serial.println("wifi: on");
    if (!g_ssid.isEmpty()) wifi_start(g_ssid, g_pass);
  } else {
    web_stop();
    clock_ntp(false);
    WiFi.disconnect(false, false);
    esp_wifi_stop();
    g_wifi = WIFI_DISABLED;
    Serial.println("wifi: off");
  }
}

static void wifi_poll() {
  static int64_t next = 0;
  const int64_t now = esp_timer_get_time() / 1000;
  if (now < next) return;
  next = now + 100;
  const wl_status_t st = WiFi.status();
  switch (g_wifi) {
    case WIFI_CONNECTING:
      if (st == WL_CONNECTED) {
        g_wifi = WIFI_UP;
        if (g_try_ssid != g_ssid || g_try_pass != g_pass) {
          g_ssid = g_try_ssid;
          g_pass = g_try_pass;
          g_prefs.putString("ssid", g_ssid);
          g_prefs.putString("pass", g_pass);
        }
        WiFi.setSleep(false);   // modem power save cuts throughput to ~50 KB/s; the PC keeps the CPU busy anyway
        Serial.printf("wifi: up, http://%s/\n", WiFi.localIP().toString().c_str());
        web_start();
        guest_net_wifi_up();
        clock_ntp(true);
      } else if (now - g_wifi_t0 > CONNECT_TIMEOUT_MS ||
                 ((st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) && now - g_wifi_t0 > 4000)) {
        Serial.printf("wifi: could not connect to %s (status %d)\n", g_try_ssid.c_str(), (int)st);
        g_attempt_failed = true;
        if (!g_ssid.isEmpty() && g_try_ssid != g_ssid) {
          wifi_start(g_ssid, g_pass);   // a new network failed: back to the saved one
        } else {
          g_wifi = WIFI_FAILED;
          g_wifi_t0 = now;
          WiFi.disconnect(false, false);
        }
      }
      break;
    case WIFI_UP:
      if (st != WL_CONNECTED) {   // lost; the driver reconnects by itself
        g_wifi = WIFI_CONNECTING;
        g_wifi_t0 = now;
        g_try_ssid = g_ssid;
        g_try_pass = g_pass;
      }
      break;
    case WIFI_FAILED:
      if (now - g_wifi_t0 > RETRY_MS && !g_ssid.isEmpty()) wifi_start(g_ssid, g_pass);
      break;
    case WIFI_NONE:
    case WIFI_DISABLED:
      break;
  }
}
