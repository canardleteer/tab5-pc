// Screenshots: the side panel's camera (side_ui.h), or serial "shot", saves the PC's picture as a PNG in
// /sdcard/screenshots, named by the day and that day's count (2026-11-05_001.png), at the picture's own size
// (720 x 400 for text, 320 x 200 for mode 13h, a VBE mode's ...), as the PC drew it: not the panel's scaled copy.
//   The picture is copied at once, in the display task (where it is drawn); a task of its own (core 0, priority 1,
//   below the display task's 2) makes the PNG and writes it, the PC running on. The camera's flash lights (at least
//   SHOT_FLASH_MS), then the camera's light shows green (saved) or red (not) for 2 s. (A white flash over the PC's
//   picture too, up to 2026-11-05: it needed a 1.4 MB copy of it, which often wasn't free beside the PC.)
//   The day comes from the Tab5's clock (the RTC, kept by its battery, in UTC; set with serial "clock" or by any
//   program that sets it), in the time zone set with serial "clock tz" (a POSIX TZ string, kept in NVS; UTC until
//   then): without a date the name is undated_001.png. (tiny386's CMOS clock stays in UTC: it reads gmtime.)
#pragma once
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <miniz.h>   // (the ESP32 ROM's: tdefl_compressor's size; the functions are M5GFX's copy, or the ROM's)

enum { SHOT_IDLE, SHOT_SAVING, SHOT_SAVED, SHOT_FAILED };
static volatile int g_shot_state = SHOT_IDLE;
static volatile int64_t g_shot_done_ms = 0;   // when SAVED / FAILED began (shown SHOT_SHOW_MS)
static constexpr int64_t SHOT_SHOW_MS = 2000, SHOT_FLASH_MS = 350;
static int64_t g_shot_start_ms = 0;          // when the picture was taken (the camera's flash: SHOT_FLASH_MS)
static uint16_t* g_shot_px = nullptr;         // the copy being saved (RGB565, w x h)
static int g_shot_w, g_shot_h;
static char g_shot_path[64];
static size_t g_shot_bytes;
static TaskHandle_t g_shot_task = nullptr;

// defined beside the renderers (tab5_386.ino): the PC's picture as it is now (RGB565, its first pixel, size, row
// length in pixels)
static bool pc_picture(const uint16_t** px, int* w, int* h, int* stride);

static bool clock_valid() { return time(nullptr) >= 1704067200; }   // (2024-01-01: the RTC was set at some time)

static void clock_print() {   // the time, UTC and local
  const time_t t = time(nullptr);
  struct tm u, l;
  gmtime_r(&t, &u);
  localtime_r(&t, &l);
  if (!clock_valid()) {
    Serial.println("clock: not set (serial: clock YYYY-MM-DD HH:MM:SS, in UTC)");
    return;
  }
  Serial.printf("clock: %04d-%02d-%02d %02d:%02d:%02d UTC, %04d-%02d-%02d %02d:%02d:%02d local (TZ %s)\n",
                u.tm_year + 1900, u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec, l.tm_year + 1900,
                l.tm_mon + 1, l.tm_mday, l.tm_hour, l.tm_min, l.tm_sec, getenv("TZ") ? getenv("TZ") : "UTC0");
}

// The Tab5's clock into the system's (time(): file dates, the names here, tiny386's CMOS clock), and the time zone
// for local time: at the start
static void clock_tz() {   // the time zone set (after setSystemTimeFromRtc, which sets TZ to GMT0)
  setenv("TZ", g_prefs.getString("tz", "UTC0").c_str(), 1);
  tzset();
}

static void clock_setup() {
  if (M5.Rtc.isEnabled()) M5.Rtc.setSystemTimeFromRtc();
  else Serial.println("clock: no RTC");
  clock_tz();
  clock_print();
}

// serial "clock" (the time), "clock YYYY-MM-DD HH:MM:SS" (the RTC and the system's set, in UTC) or "clock tz <TZ>"
// (the time zone, a POSIX TZ string such as PST8PDT,M3.2.0,M11.1.0 or CET-1CEST,M3.5.0,M10.5.0/3; kept)
static void clock_command(const String& arg) {
  int Y, M, D, h, m, s;
  if (arg.startsWith("tz ")) {
    g_prefs.putString("tz", arg.substring(3));
    clock_tz();
  } else if (arg.length() && sscanf(arg.c_str(), "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &s) == 6) {
    m5::rtc_datetime_t dt;
    dt.date.year = Y, dt.date.month = M, dt.date.date = D;
    dt.time.hours = h, dt.time.minutes = m, dt.time.seconds = s;
    xSemaphoreTake(g_i2c_lock, portMAX_DELAY);
    M5.Rtc.setDateTime(dt);
    M5.Rtc.setSystemTimeFromRtc();
    xSemaphoreGive(g_i2c_lock);
    clock_tz();
  }
  clock_print();
}

// the next free name in /sdcard/screenshots for today: its count one past the highest there
static bool shot_next_path(char* out, size_t n) {
  static const char* const DIR_ = "/sdcard/screenshots";
  if (mkdir(DIR_, 0777) != 0 && errno != EEXIST) return false;
  char day[16] = "undated";
  if (clock_valid()) {
    const time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(day, sizeof(day), "%Y-%m-%d", &tm);
  }
  const size_t dl = strlen(day);
  int top = 0;
  if (DIR* d = opendir(DIR_)) {
    while (dirent* e = readdir(d)) {   // day_NNN.png (FatFs: the case as written)
      const char* nm = e->d_name;
      if (strncasecmp(nm, day, dl) == 0 && nm[dl] == '_') top = max(top, atoi(nm + dl + 1));
    }
    closedir(d);
  }
  snprintf(out, n, "%s/%s_%03d.png", DIR_, day, top + 1);
  return true;
}

// The PNG written as it is made: the picture's rows (RGB888, filter 0) through miniz's deflate, its output gathered
// into IDAT chunks of up to PNG_CHUNK bytes as it comes. (Not miniz's in-memory PNG writer: it holds the whole file,
// as big as the picture uncompressed to start with: 1.4 MB for 800 x 600, more than was free beside an 18 MB PC.)
static constexpr size_t PNG_CHUNK = 32768;
struct PngOut {
  FILE* f;
  uint8_t* buf;
  size_t n;
  bool ok;
  size_t total;
};

static uint32_t png_crc(uint32_t c, const uint8_t* p, size_t n) {   // CRC-32 (IEEE), as PNG's chunks have it
  static uint32_t table[256];
  if (!table[1])
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t k = i;
      for (int b = 0; b < 8; ++b) k = k & 1 ? 0xEDB88320u ^ k >> 1 : k >> 1;
      table[i] = k;
    }
  c = ~c;
  while (n--) c = table[(c ^ *p++) & 0xFF] ^ c >> 8;
  return ~c;
}

static void png_chunk(PngOut& o, const char* type, const uint8_t* data, size_t len) {
  if (!o.ok) return;
  const uint8_t head[8] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len,
                           (uint8_t)type[0], (uint8_t)type[1], (uint8_t)type[2], (uint8_t)type[3]};
  const uint32_t crc = png_crc(png_crc(0, head + 4, 4), data, len);
  const uint8_t tail[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc};
  o.ok = fwrite(head, 1, 8, o.f) == 8 && (!len || fwrite(data, 1, len, o.f) == len) && fwrite(tail, 1, 4, o.f) == 4;
  o.total += 12 + len;
}

static mz_bool png_put(const void* p, int len, void* user) {   // deflate's output, into IDAT chunks
  PngOut& o = *(PngOut*)user;
  const uint8_t* q = (const uint8_t*)p;
  while (len > 0 && o.ok) {
    const size_t k = min((size_t)len, PNG_CHUNK - o.n);
    memcpy(o.buf + o.n, q, k);
    o.n += k, q += k, len -= k;
    if (o.n == PNG_CHUNK) {
      png_chunk(o, "IDAT", o.buf, o.n);
      o.n = 0;
    }
  }
  return o.ok;
}

// g_shot_px (w x h) as a PNG at path; false if it couldn't be (the file removed then)
static bool png_write(const char* path, int w, int h) {
  PngOut o = {fopen(path, "wb"), (uint8_t*)heap_caps_malloc(PNG_CHUNK, MALLOC_CAP_SPIRAM), 0, true, 8};
  uint8_t* row = (uint8_t*)heap_caps_malloc((size_t)w * 3 + 1, MALLOC_CAP_SPIRAM);
  tdefl_compressor* z = (tdefl_compressor*)heap_caps_malloc(sizeof(tdefl_compressor), MALLOC_CAP_SPIRAM);
  o.ok = o.f && o.buf && row && z;
  if (o.ok) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    const uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                              (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h,
                              8, 2, 0, 0, 0};   // 8 bits a channel, RGB, deflate, filters by row, not interlaced
    o.ok = fwrite(sig, 1, 8, o.f) == 8;
    png_chunk(o, "IHDR", ihdr, 13);
    // 32 probes: miniz's level 3 (as small as level 6 for a PC's flat colours, in about half the time)
    o.ok = o.ok && tdefl_init(z, png_put, &o, 32 | TDEFL_WRITE_ZLIB_HEADER) == TDEFL_STATUS_OKAY;
    for (int y = 0; y < h && o.ok; ++y) {
      const uint16_t* src = g_shot_px + (size_t)y * w;
      row[0] = 0;   // (filter: none)
      for (int x = 0; x < w; ++x) {
        const uint16_t px = src[x];
        const uint8_t r = px >> 11, g = px >> 5 & 63, b = px & 31;
        row[1 + 3 * x] = r << 3 | r >> 2;
        row[2 + 3 * x] = g << 2 | g >> 4;
        row[3 + 3 * x] = b << 3 | b >> 2;
      }
      o.ok = tdefl_compress_buffer(z, row, (size_t)w * 3 + 1, TDEFL_NO_FLUSH) == TDEFL_STATUS_OKAY && o.ok;
    }
    o.ok = o.ok && tdefl_compress_buffer(z, nullptr, 0, TDEFL_FINISH) == TDEFL_STATUS_DONE;
    if (o.n) png_chunk(o, "IDAT", o.buf, o.n);
    png_chunk(o, "IEND", nullptr, 0);
  }
  if (o.f) o.ok = fclose(o.f) == 0 && o.ok;
  heap_caps_free(z);
  heap_caps_free(row);
  heap_caps_free(o.buf);
  if (!o.ok && o.f) unlink(path);
  if (o.ok) g_shot_bytes = o.total;
  return o.ok;
}

static void shot_task(void*) {
  const int64_t t0 = esp_timer_get_time();
  const bool named = shot_next_path(g_shot_path, sizeof(g_shot_path));
  const bool ok = named && png_write(g_shot_path, g_shot_w, g_shot_h);
  heap_caps_free(g_shot_px);
  g_shot_px = nullptr;
  if (ok)
    Serial.printf("shot: %s, %d x %d, %u bytes, %lld ms\n", g_shot_path, g_shot_w, g_shot_h, (unsigned)g_shot_bytes,
                  (esp_timer_get_time() - t0) / 1000);
  else
    Serial.printf("shot: not saved (%s; PSRAM %u free)\n", named ? "the card full or missing, or no memory" : "no folder",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  g_shot_done_ms = esp_timer_get_time() / 1000;
  g_shot_state = ok ? SHOT_SAVED : SHOT_FAILED;
  g_shot_task = nullptr;
  vTaskDelete(nullptr);
}

// The picture copied and its saving started (the display task: the button's release, serial "shot")
static void shot_start(int64_t now_ms) {
  if (g_shot_state == SHOT_SAVING || g_shot_task) return;
  const uint16_t* src;
  int w, h, stride;
  if (!pc_picture(&src, &w, &h, &stride) || w <= 0 || h <= 0) {
    Serial.println("shot: no picture");
    return;
  }
  g_shot_px = (uint16_t*)heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
  if (!g_shot_px) {
    Serial.println("shot: not enough memory");
    g_shot_done_ms = now_ms;
    g_shot_state = SHOT_FAILED;
    return;
  }
  for (int y = 0; y < h; ++y) memcpy(g_shot_px + (size_t)y * w, src + (size_t)y * stride, (size_t)w * 2);
  g_shot_w = w, g_shot_h = h;
  g_shot_state = SHOT_SAVING;
  g_shot_start_ms = now_ms;
  // the stack in PSRAM (internal RAM is short with Wi-Fi on), as the installers' tasks
  if (xTaskCreatePinnedToCoreWithCaps(shot_task, "shot", 8192, nullptr, 1, &g_shot_task, 0, MALLOC_CAP_SPIRAM) !=
      pdPASS) {
    g_shot_task = nullptr;
    heap_caps_free(g_shot_px);
    g_shot_px = nullptr;
    Serial.println("shot: not enough memory for its task");
    g_shot_done_ms = now_ms;
    g_shot_state = SHOT_FAILED;
  }
}

static int shot_status(int64_t now_ms) {   // for the camera: SAVING (at least SHOT_FLASH_MS), SAVED / FAILED for
  const int s = g_shot_state;              // SHOT_SHOW_MS, then IDLE
  if (s != SHOT_IDLE && now_ms - g_shot_start_ms < SHOT_FLASH_MS) return SHOT_SAVING;
  if ((s == SHOT_SAVED || s == SHOT_FAILED) && now_ms - g_shot_done_ms >= SHOT_SHOW_MS) {
    g_shot_state = SHOT_IDLE;
    return SHOT_IDLE;
  }
  return s;
}
