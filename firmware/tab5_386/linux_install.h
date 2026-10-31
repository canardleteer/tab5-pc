// Machines, and installing DOS or Linux as one, from the Tab5 itself.
//
// A machine is a settings file at the card's root: /tiny386.ini ("default"), or /tiny386-N.ini, a machine named N,
// whose files are in /sdcard/N. The settings panel lists them by name and starts the one chosen: remembered (NVS
// "machine"), read at every start instead of /tiny386.ini (load_config; /tiny386.ini again if the file has gone).
// Its Install DOS and Install Linux ask for a name and make such a machine: FreeDOS (fd_install.h), or Sprig Linux.
//
// Sprig Linux (github.com/yejun/sprig-linux): the latest release (GitHub's redirect from releases/latest), its files
// downloaded into /sdcard/N (root.img.gz unpacked as it comes: the ROM's tinfl, as fd_install.h), each checked against
// the release's SHA256SUMS; /tiny386-N.ini from the release's graphics console settings. A name already holding
// Sprig Linux is updated: its root.img holds the user's files and is kept, only the kernels and the loader replaced.
#pragma once
#include <dirent.h>
#include <mbedtls/sha256.h>

static String g_machine;   // the machine's file at the card's root, as chosen (NVS "machine"; "": /tiny386.ini)
static constexpr int MAX_MACHINES = 16;
struct Machine { char file[48]; char name[40]; };
static Machine g_machines[MAX_MACHINES];
static int g_machine_count = 0;

// The card's machines: default (tiny386.ini) first, then the named ones by name
static void machines_scan() {
  g_machine_count = 0;
  DIR* d = opendir("/sdcard");
  if (!d) return;
  while (dirent* e = readdir(d)) {
    const size_t n = strlen(e->d_name);
    if (e->d_type == DT_DIR || n >= sizeof(Machine::file) || g_machine_count == MAX_MACHINES) continue;
    const bool dflt = !strcasecmp(e->d_name, "tiny386.ini");
    if (!dflt && (n < 13 || strncasecmp(e->d_name, "tiny386-", 8) || strcasecmp(e->d_name + n - 4, ".ini"))) continue;
    Machine& m = g_machines[g_machine_count++];
    snprintf(m.file, sizeof(m.file), "%s", e->d_name);
    if (dflt) snprintf(m.name, sizeof(m.name), "default");
    else snprintf(m.name, sizeof(m.name), "%.*s", (int)n - 12, e->d_name + 8);
  }
  closedir(d);
  std::sort(g_machines, g_machines + g_machine_count, [](const Machine& a, const Machine& b) {
    const bool ta = !strcasecmp(a.file, "tiny386.ini"), tb = !strcasecmp(b.file, "tiny386.ini");
    return ta != tb ? ta : strcasecmp(a.name, b.name) < 0;
  });
}

// A new machine's name: letters, digits, - and _ (it names a file and a folder), not "default"
static bool machine_name_ok(const char* n) {
  const size_t len = strlen(n);
  if (!len || len > 24 || !strcasecmp(n, "default")) return false;
  for (const char* p = n; *p; ++p)
    if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') return false;
  return true;
}

static bool path_exists(const String& p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}

// The name in use: its settings file or its folder is on the card
static bool machine_name_used(const char* n) {
  return path_exists("/sdcard/tiny386-" + String(n) + ".ini") || path_exists("/sdcard/" + String(n));
}

// The settings file the PC starts with: the chosen machine's, if it is still there
static String machine_path() {
  if (g_machine.length()) {
    const String p = "/sdcard/" + g_machine;
    struct stat st;
    if (stat(p.c_str(), &st) == 0) return p;
  }
  return "/sdcard/tiny386.ini";
}

static void machine_choose(const char* file) {   // from the next start (the caller restarts)
  g_machine = strcasecmp(file, "tiny386.ini") ? file : "";
  g_prefs.putString("machine", g_machine);
}

// ---- Sprig Linux -----------------------------------------------------------------------------------------------

static constexpr const char* LX_RELEASES = "https://github.com/yejun/sprig-linux/releases";
// The pieces asked for: small, as what is on its way waits in DMA-capable internal RAM (lwIP's buffers; its TCP window
// is 64 KB), of which a running PC leaves ~8 KB: with 256 KB pieces it all went, and the AES unit decrypting TLS
// couldn't have its DMA descriptors ("esp-aes: Failed to allocate memory for the array of DMA descriptors")
static constexpr int LX_CHUNK = 16 << 10;
static int g_lx_retries;   // (pieces asked for again, for the log)
static String g_lx_name;   // the machine being installed: /sdcard/<name>, /sdcard/tiny386-<name>.ini
static volatile FdState g_lx_state = FD_IDLE;
static volatile int g_lx_pct = -1;   // the download's progress (-1: no bar)
static char g_lx_msg[160] = "";
static char g_lx_location[256];      // a redirect's target (lx_http_event)

static void lx_say(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_lx_msg, sizeof(g_lx_msg), fmt, ap);
  va_end(ap);
  Serial.printf("linux: %s\n", g_lx_msg);
}

// Sprig Linux is the machine named n (its root.img and the release's VERSION there): installing it again updates it
static bool lx_installed(const char* n) {
  return path_exists("/sdcard/" + String(n) + "/root.img") && path_exists("/sdcard/" + String(n) + "/VERSION");
}

static esp_err_t lx_http_event(esp_http_client_event_t* e) {
  if (e->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(e->header_key, "Location"))
    snprintf(g_lx_location, sizeof(g_lx_location), "%s", e->header_value);
  return fd_http_event(e);   // (Content-Range: g_fd_total)
}

static esp_http_client_handle_t lx_client(const char* url, bool redirects) {
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.keep_alive_enable = true;
  cfg.timeout_ms = 20000;
  cfg.event_handler = lx_http_event;
  cfg.buffer_size = 8192;      // (GitHub's answers have long header lines)
  cfg.buffer_size_tx = 2048;   // (and its redirects long URLs: the assets' signed ones)
  cfg.disable_auto_redirect = !redirects;
  return esp_http_client_init(&cfg);
}

// The latest release's tag (GitHub answers .../releases/latest with a redirect to .../releases/tag/<tag>)
static bool lx_latest(char* tag, size_t cap) {
  esp_http_client_handle_t c = lx_client((String(LX_RELEASES) + "/latest").c_str(), false);
  if (!c) return false;
  g_lx_location[0] = 0;
  bool ok = false;
  if (esp_http_client_open(c, 0) == ESP_OK) {
    esp_http_client_fetch_headers(c);
    const int status = esp_http_client_get_status_code(c);
    const char* t = strstr(g_lx_location, "/releases/tag/");
    if (status / 100 == 3 && t && strlen(t + 14) > 0 && strlen(t + 14) < cap) {
      snprintf(tag, cap, "%s", t + 14);
      ok = true;
    } else {
      Serial.printf("linux: latest: HTTP %d, %s\n", status, g_lx_location);
    }
  }
  esp_http_client_cleanup(c);
  return ok;
}

// A range of the release's file at url into buf, whole; its length, or -1. GitHub answers with a redirect to the
// file's own address (a signed one, good for a few minutes), followed here (the client's open/read calls don't follow
// redirects) and gone on with for the next ranges; a failure starts again from url.
static int lx_get(esp_http_client_handle_t c, const char* url, const char* range, uint8_t* buf, int cap) {
  char h[48];
  snprintf(h, sizeof(h), "bytes=%s", range);
  for (int attempt = 0; attempt < 4; ++attempt) {
    esp_http_client_set_header(c, "Range", h);
    for (int hop = 0; hop < 5; ++hop) {
      if (esp_http_client_open(c, 0) != ESP_OK) break;
      const int64_t len = esp_http_client_fetch_headers(c);
      const int status = esp_http_client_get_status_code(c);
      if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        esp_http_client_set_redirection(c);
        esp_http_client_close(c);
        continue;
      }
      if (status == 206 && len > 0 && len <= cap) {
        int got = 0;
        while (got < len) {
          const int n = esp_http_client_read(c, (char*)buf + got, len - got);
          if (n <= 0) break;
          got += n;
        }
        if (got == len) return got;   // (all of it read: the connection can go on to the next)
      }
      Serial.printf("linux: %s: HTTP %d, %lld bytes (DMA-capable internal RAM %u free, largest %u)\n", h, status,
                    (long long)len, (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
      break;
    }
    esp_http_client_close(c);
    esp_http_client_set_url(c, url);
    ++g_lx_retries;
    vTaskDelay(pdMS_TO_TICKS(500 << attempt));
  }
  return -1;
}

struct LxSum { char name[40]; uint8_t sha[32]; };

static int hexval(char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : (ch | 32) >= 'a' && (ch | 32) <= 'f' ? (ch | 32) - 'a' + 10 : -1; }

// SHA256SUMS ("<64 hex>  <name>" lines) into sums; their number
static int lx_parse_sums(const char* text, LxSum* sums, int cap) {
  int n = 0;
  for (const char* p = text; *p && n < cap;) {
    const char* e = strchr(p, '\n');
    const size_t len = e ? e - p : strlen(p);
    if (len > 66 && p[64] == ' ') {
      LxSum& s = sums[n];
      bool ok = true;
      for (int i = 0; i < 32 && ok; ++i) {
        const int h = hexval(p[2 * i]), l = hexval(p[2 * i + 1]);
        ok = h >= 0 && l >= 0;
        s.sha[i] = h << 4 | l;
      }
      const char* name = p + 66 - (p[65] != ' ' && p[65] != '*');
      const size_t nl = len - (name - p);
      if (ok && nl < sizeof(s.name)) {
        memcpy(s.name, name, nl);
        s.name[nl] = 0;
        ++n;
      }
    }
    p += len + (e ? 1 : 0);
  }
  return n;
}

struct LxCtx {
  esp_http_client_handle_t c;
  const char* tag;
  uint8_t* buf;
  LxSum sums[16];
  int nsums;
  uint64_t done_bytes, all_bytes;   // the progress over all the downloads
};

// The release's file `name`, piece by piece to `piece(data, n, offset, total)`; checked against SHA256SUMS
static bool lx_fetch(LxCtx& x, const char* name, bool (*piece)(LxCtx&, const uint8_t*, int, uint64_t, uint64_t, void*),
                     void* arg) {
  const LxSum* want = nullptr;
  for (int i = 0; i < x.nsums; ++i)
    if (!strcmp(x.sums[i].name, name)) want = &x.sums[i];
  if (!want) { lx_say("The release has no checksum for %s", name); return false; }
  const String url = String(LX_RELEASES) + "/download/" + x.tag + "/" + name;
  esp_http_client_set_url(x.c, url.c_str());
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  bool ok = true;
  g_fd_total = 0;
  for (uint64_t at = 0; ok && (at == 0 || at < g_fd_total);) {
    char r[48];
    snprintf(r, sizeof(r), "%llu-%llu", (unsigned long long)at, (unsigned long long)(at + LX_CHUNK - 1));
    const int n = lx_get(x.c, url.c_str(), r, x.buf, LX_CHUNK);
    if (n <= 0 || !g_fd_total) { lx_say("Download failed (%s): the connection was lost", name); ok = false; break; }
    mbedtls_sha256_update(&sha, x.buf, n);
    ok = piece(x, x.buf, n, at, g_fd_total, arg);
    at += n;
    x.done_bytes += n;
    if (x.all_bytes) g_lx_pct = (int)std::min<uint64_t>(99, x.done_bytes * 100 / x.all_bytes);
  }
  uint8_t got[32];
  mbedtls_sha256_finish(&sha, got);
  mbedtls_sha256_free(&sha);
  if (ok && memcmp(got, want->sha, 32)) { lx_say("%s is damaged (its checksum differs)", name); ok = false; }
  return ok;
}

static void lx_say_progress(const char* tag) {   // (to the log only when the percentage changes)
  static int last = -2;
  const int pct = std::max(0, (int)g_lx_pct);
  if (pct == last) return;
  last = pct;
  lx_say("Downloading Sprig Linux %s: %d%%", tag, pct);
}

static bool lx_write(FILE* f, const uint8_t* p, size_t n) {   // (from PSRAM: the card's DMA reads it)
  JitHold hold;   // (tab5_386.ino: core 0's cache operations on PSRAM)
  esp_cache_msync((void*)p, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  return fwrite(p, 1, n, f) == n;
}

static bool lx_to_file(LxCtx& x, const uint8_t* p, int n, uint64_t at, uint64_t total, void* arg) {
  lx_say_progress(x.tag);
  if (!lx_write((FILE*)arg, p, n)) { lx_say("Writing to the card failed (card full?)"); return false; }
  return true;
}

static bool lx_to_string(LxCtx& x, const uint8_t* p, int n, uint64_t at, uint64_t total, void* arg) {
  String& s = *(String*)arg;
  if (s.length() + n > 16384) { lx_say("A settings file of the release is too long"); return false; }
  s.concat((const char*)p, n);
  return true;
}

// root.img.gz, unpacked as it comes into root.img (made in one piece beforehand: the PC's raw card access)
struct LxGunzip {
  FILE* out;
  uint8_t* dict;
  tinfl_decompressor* inf;
  size_t dict_ofs;
  uint32_t crc, written, want_crc, want_size;
  bool header_done, done;
};
static bool lx_gunzip(LxCtx& x, const uint8_t* p, int n, uint64_t at, uint64_t total, void* arg) {
  LxGunzip& g = *(LxGunzip*)arg;
  lx_say_progress(x.tag);
  size_t in_ofs = 0;
  if (!g.header_done) {   // (the gzip header, all in the first piece)
    if (n < 10 || p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) { lx_say("root.img.gz isn't gzip"); return false; }
    const uint8_t fl = p[3];
    in_ofs = 10;
    if (fl & 4) in_ofs += 2 + le16(p + in_ofs);
    if (fl & 8) while (in_ofs < (size_t)n && p[in_ofs++]) {}
    if (fl & 16) while (in_ofs < (size_t)n && p[in_ofs++]) {}
    if (fl & 2) in_ofs += 2;
    g.header_done = true;
  }
  const bool more = at + n < total;
  while (!g.done && in_ofs <= (size_t)n) {
    size_t in_n = n - in_ofs, out_n = TINFL_LZ_DICT_SIZE - g.dict_ofs;
    const tinfl_status st = tinfl_decompress(g.inf, p + in_ofs, &in_n, g.dict, g.dict + g.dict_ofs, &out_n,
                                             more ? TINFL_FLAG_HAS_MORE_INPUT : 0);
    in_ofs += in_n;
    if (out_n) {
      g.crc = fd_crc32(g.crc, g.dict + g.dict_ofs, out_n);
      if (g.written + out_n > g.want_size || !lx_write(g.out, g.dict + g.dict_ofs, out_n)) {
        lx_say("Writing root.img failed (card full?)");
        return false;
      }
      g.written += out_n;
      g.dict_ofs = (g.dict_ofs + out_n) & (TINFL_LZ_DICT_SIZE - 1);
    }
    if (st == TINFL_STATUS_DONE) g.done = true;
    else if (st < 0) { lx_say("root.img.gz is damaged"); return false; }
    else if (st == TINFL_STATUS_NEEDS_MORE_INPUT) break;
  }
  return true;
}

static bool lx_replace(const String& part, const String& final) {
  unlink(final.c_str());
  return rename(part.c_str(), final.c_str()) == 0;
}

// The release's settings (its graphics console's), as the machine: its files in /sdcard/<name>
static bool lx_machine(const String& ini, const char* tag) {
  const String dir = "/sdcard/" + g_lx_name + "/";
  String s = ini;
  s.replace("/sdcard/linux/", dir);
  s.replace("in /linux on the card", "in /" + g_lx_name + " on the card");
  s = String("; Sprig Linux ") + tag + ", installed by the Tab5 as the machine " + g_lx_name + ".\r\n" +
      "; The text console instead (faster, 80 x 25): kernel = " + dir + "bzImage, and in cmdline leave out\r\n" +
      "; vga=0x303 video=vesafb:ypan.\r\n" + s;
  if (!s.endsWith("\n")) s += "\r\n";
  return fd_write_file(("/sdcard/tiny386-" + g_lx_name + ".ini").c_str(), s.c_str());
}

static bool lx_install_run() {
  LxCtx x = {};
  char tag[32] = "";
  LxGunzip g = {};
  FILE* f = nullptr;
  const bool update = lx_installed(g_lx_name.c_str());
  const String dir = "/sdcard/" + g_lx_name;
  x.buf = (uint8_t*)heap_caps_aligned_alloc(128, LX_CHUNK, MALLOC_CAP_SPIRAM);
  g.dict = (uint8_t*)heap_caps_aligned_calloc(128, 1, TINFL_LZ_DICT_SIZE, MALLOC_CAP_SPIRAM);
  g.inf = (tinfl_decompressor*)heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
  static const char* const files[] = {"linuxstart.bin", "bzImage", "bzImage-fb", "BUILDINFO"};
  bool ok = false;
  do {
    g_lx_retries = 0;
    if (!x.buf || !g.dict || !g.inf) { lx_say("Not enough memory"); break; }
    Serial.printf("linux: DMA-capable internal RAM %u free (largest %u), internal %u, PSRAM %u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    lx_say("Asking GitHub for the latest Sprig Linux ...");
    if (!lx_latest(tag, sizeof(tag))) { lx_say("Could not reach github.com"); break; }
    x.tag = tag;
    x.c = lx_client(LX_RELEASES, true);
    if (!x.c) { lx_say("Not enough memory for the download"); break; }
    mkdir(dir.c_str(), 0777);

    String sums, ini_fb;
    {   // (SHA256SUMS itself: nothing to check it against but HTTPS, from the release on GitHub)
      const String url = String(LX_RELEASES) + "/download/" + tag + "/SHA256SUMS";
      esp_http_client_set_url(x.c, url.c_str());
      g_fd_total = 0;
      const int n = lx_get(x.c, url.c_str(), "0-16383", x.buf, LX_CHUNK);
      if (n <= 0) { lx_say("Download failed (SHA256SUMS)"); break; }
      sums.concat((const char*)x.buf, n);
      x.nsums = lx_parse_sums(sums.c_str(), x.sums, 16);
    }
    // the sizes, for the progress (each file's total comes with its first piece; root.img.gz's from its tail)
    if (!update) {
      const String url = String(LX_RELEASES) + "/download/" + tag + "/root.img.gz";
      esp_http_client_set_url(x.c, url.c_str());
      g_fd_total = 0;   // (its size from a first byte's answer: GitHub's file server takes no "last n bytes" range)
      if (lx_get(x.c, url.c_str(), "0-0", x.buf, LX_CHUNK) != 1 || g_fd_total < 18) {
        lx_say("Download failed (root.img.gz)");
        break;
      }
      char r[48];
      snprintf(r, sizeof(r), "%llu-%llu", (unsigned long long)g_fd_total - 8, (unsigned long long)g_fd_total - 1);
      if (lx_get(x.c, url.c_str(), r, x.buf, LX_CHUNK) != 8) { lx_say("Download failed (root.img.gz)"); break; }
      g.want_crc = le32(x.buf);
      g.want_size = le32(x.buf + 4);
      x.all_bytes = g_fd_total;
    }
    x.all_bytes += 6 << 20;   // (the kernels and the rest, about)
    if (!lx_fetch(x, "tiny386-linux-fb.ini", lx_to_string, &ini_fb)) break;
    bool bad = false;
    for (const char* name : files) {
      const String part = dir + "/" + name + ".part";
      if (!(f = fopen(part.c_str(), "wb"))) { lx_say("Can't write to the card"); bad = true; break; }
      bad = !lx_fetch(x, name, lx_to_file, f);
      if (fclose(f) != 0) bad = true;
      f = nullptr;
      if (bad) break;
    }
    if (bad) break;
    if (!update) {   // root.img, in one piece (raw access: disk_access.h)
      const String part = dir + "/root.img.part";
      unlink(part.c_str());
      if (esp_vfs_fat_create_contiguous_file("/sdcard", part.c_str(), g.want_size, false) != ESP_OK ||
          !(g.out = fopen(part.c_str(), "r+b"))) {
        lx_say("No room for root.img (%u MB) on the card", (unsigned)(g.want_size >> 20));
        break;
      }
      tinfl_init(g.inf);
      const bool fetched = lx_fetch(x, "root.img.gz", lx_gunzip, &g);
      if (fclose(g.out) != 0 || !fetched) { if (fetched) lx_say("Writing root.img failed"); break; }
      if (!g.done || g.written != g.want_size || g.crc != g.want_crc) { lx_say("root.img.gz is damaged"); break; }
    }
    // all there and checked: in place
    for (const char* name : files) bad |= !lx_replace(dir + "/" + name + ".part", dir + "/" + name);
    if (!update) bad |= !lx_replace(dir + "/root.img.part", dir + "/root.img");
    bad |= !lx_machine(ini_fb, tag) || !fd_write_file((dir + "/VERSION").c_str(), (String(tag) + "\r\n").c_str());
    if (bad) { lx_say("Writing to the card failed"); break; }
    ok = true;
  } while (false);
  if (x.c) esp_http_client_cleanup(x.c);
  if (!ok) {
    for (const char* name : files) unlink((dir + "/" + name + ".part").c_str());
    unlink((dir + "/root.img.part").c_str());
  }
  free(x.buf);
  free(g.dict);
  free(g.inf);
  Serial.printf("linux: %d pieces asked for again\n", g_lx_retries);
  if (ok) {
    g_lx_pct = -1;
    lx_say(update ? "Updated %s to Sprig Linux %s (its files kept): from its next start."
                  : "Installed Sprig Linux %s as %s. Choose it above and tap Start.",
           update ? g_lx_name.c_str() : tag, update ? tag : g_lx_name.c_str());
  }
  return ok;
}

static bool lx_install_psram_tls() {   // (as fd_download_psram_tls)
  mbedtls_platform_set_calloc_free(fd_tls_calloc, free);
  const bool ok = lx_install_run();
  mbedtls_platform_set_calloc_free(fd_idf_tls_calloc, fd_idf_tls_free);
  return ok;
}

static TaskHandle_t g_lx_task = nullptr;
static void lx_install_task(void*) {   // (as fd_install_task: its stack in PSRAM)
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    g_lx_state = lx_install_psram_tls() ? FD_IDLE : FD_FAILED;
    machines_scan();
  }
}

// Install DOS ('d'), Linux ('l') or from an installer CD ('i', name "<name>:<the .iso at the card's root>") as the
// machine name: the Tab5 restarts and does it before any PC starts (install_mode_run, the .ino), then restarts into it
static void restart_tab5();   // (web.h)
static void install_request(char kind, const char* name) {
  g_prefs.putString("install", String(kind) + ":" + name);
  restart_tab5();
}

static void lx_install_start(const char* name) {
  if (g_lx_state == FD_RUNNING || g_fd_state == FD_RUNNING) return;
  g_lx_name = name;
  if (!g_lx_task && xTaskCreatePinnedToCoreWithCaps(lx_install_task, "lxinstall", 24576, nullptr, 1, &g_lx_task, 0,
                                                    MALLOC_CAP_SPIRAM) != pdPASS) {
    g_lx_task = nullptr;
    g_lx_state = FD_FAILED;
    lx_say("Not enough memory to start");
    return;
  }
  g_lx_state = FD_RUNNING;
  g_lx_pct = 0;
  lx_say("Starting ...");
  xTaskNotifyGive(g_lx_task);
}
