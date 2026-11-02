// FreeDOS on an empty card, from the Tab5 itself (the settings panel's "Install FreeDOS", once Wi-Fi is up): FreeDOS
// 1.4's LiteUSB disk (its installer) is downloaded from freedos.org, a blank C.IMG made, and the PC started with the
// installer as its first disk, C.IMG as its second (drive D: to the installer, as a PC's own disk is when it starts
// from a USB stick) and an empty CD-ROM drive (the installer stops with "CD-ROM not configured" without one). The user
// goes through FreeDOS's own installer; when the PC restarts with FreeDOS on C.IMG, it starts from C.IMG alone.
// Or, from an installer CD's .iso at the card's root (the panel's Install ISO; no download): a blank C.IMG, and the PC
// starts from the CD while C.IMG has no system.
//
// The download comes in 256 KB ranges, each received whole before anything is written to the card (the card and the
// Wi-Fi chip share the SDMMC host: see web.h, h_put), and is unzipped as it comes (the ROM's tinfl), its CRC checked.
#pragma once
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <miniz.h>   // (the ROM's tinfl)
#include <mbedtls/platform.h>
#include "mbr_code.h"   // (tools/mbr/mbr.S)
// ESP-IDF's mbedTLS allocator (internal RAM, as the Arduino core has it), by its C name (esp_mem.h has no C++ guards)
void* fd_idf_tls_calloc(size_t n, size_t size) __asm__("esp_mbedtls_mem_calloc");
void fd_idf_tls_free(void* p) __asm__("esp_mbedtls_mem_free");

static constexpr const char* FD_URL = "https://download.freedos.org/1.4/FD14-LiteUSB.zip";
static constexpr const char* FD_ENTRY = "FD14LITE.img";   // the LiteUSB disk in the zip
// Where an install goes (fd_use): for the default machine the card's root and /sdcard/tiny386.ini; for one named N,
// /sdcard/N and /sdcard/tiny386-N.ini (the settings panel's machines: linux_install.h)
static String g_fd_dir = "/sdcard", g_fd_ini = "/sdcard/tiny386.ini", g_fd_setup, g_fd_disk, g_fd_nocd;
static String g_fd_machine;   // the machine's file to start after the install's restart ("": tiny386.ini)
#define FD_SETUP g_fd_setup.c_str()
#define FD_DISK g_fd_disk.c_str()
#define FD_NOCD g_fd_nocd.c_str()
#define FD_INI g_fd_ini.c_str()
static void fd_use(const String& ini) {   // the machine whose settings are in ini (a path on the card)
  g_fd_ini = ini;
  const String file = ini.substring(ini.lastIndexOf('/') + 1);
  const bool named = file.startsWith("tiny386-") && file.endsWith(".ini") && file.length() > 12;
  g_fd_dir = named ? "/sdcard/" + file.substring(8, file.length() - 4) : String("/sdcard");
  g_fd_machine = named ? file : String("");
  g_fd_setup = g_fd_dir + "/FDSETUP.IMG";
  g_fd_disk = g_fd_dir + "/C.IMG";
  g_fd_nocd = g_fd_dir + "/NOCD.ISO";
}
// a settings text with @/ for the install's directory
static String fd_ini_text(const char* t) {
  String s = t;
  s.replace("@/", g_fd_dir + "/");
  return s;
}
static constexpr uint64_t FD_DISK_SIZE = 512ull << 20;
static constexpr int FD_CHUNK = 256 << 10;    // the download's buffer: a piece (and the zip's central directory: its tail)
// The pieces asked for: each costs a round trip to the server, and with 16 KB ones (as when this ran beside the PC,
// which left lwIP ~8 KB of DMA-capable internal RAM) the download went at ~50 KB/s. Installs run with no PC now
// (install_mode_run), and what is on its way is bounded by lwIP's TCP window (64 KB).
static constexpr int FD_PIECE = 256 << 10;
static constexpr int FD_DISK_HEAD = 320 * 512;   // C.IMG's sectors with data: up to 63 + 1 + 252 (fd_make_disk)

enum FdState { FD_IDLE, FD_RUNNING, FD_FAILED };
static volatile FdState g_fd_state = FD_IDLE;
static volatile int g_fd_pct = 0;       // the download's progress
static char g_fd_msg[120] = "";         // what it is doing, or why it failed (the settings panel shows it)
static bool g_setup_needed = false;     // no disks on the card (load_config): the panel opens, to install a machine
// tiny386.ini's [tab5] install: 1, FreeDOS's installer is running (from FDSETUP.IMG); 2, the PC starts from an
// installer CD, and C.IMG has no system yet. (Installs ran with the translator off for a while: two of five had stopped partway, from the code sync fixed since,
// src/t386_i386.c.)
static int g_install_mode = 0;
// The installer CD images at the card's root (fd_find_isos: the panel's Install ISO), and the install's own CD image
// ("": FreeDOS's download)
static constexpr int FD_MAX_ISOS = 8;
static String g_fd_isos[FD_MAX_ISOS];
static int g_fd_iso_count = 0;
static String g_fd_iso;

static void fd_say(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_fd_msg, sizeof(g_fd_msg), fmt, ap);
  va_end(ap);
  Serial.printf("install: %s\n", g_fd_msg);
}

// ---- the zip, by HTTP ranges ---------------------------------------------------------------------------------

static uint64_t g_fd_total;   // the zip's size (Content-Range)
static esp_err_t fd_http_event(esp_http_client_event_t* e) {
  if (e->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(e->header_key, "Content-Range")) {
    const char* slash = strchr(e->header_value, '/');
    if (slash) g_fd_total = strtoull(slash + 1, nullptr, 10);
  }
  return ESP_OK;
}

// bytes `range` ("a-b", or "-n": the last n) into buf, whole; their number, or -1
static int fd_get(esp_http_client_handle_t c, const char* range, uint8_t* buf, int cap) {
  for (int attempt = 0; attempt < 4; ++attempt) {
    char h[48];
    snprintf(h, sizeof(h), "bytes=%s", range);
    esp_http_client_set_header(c, "Range", h);
    const esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) Serial.printf("install: %s: %s\n", h, esp_err_to_name(err));
    if (err == ESP_OK) {
      const int64_t len = esp_http_client_fetch_headers(c);
      const int status = esp_http_client_get_status_code(c);
      if (status != 206 || len <= 0 || len > cap)
        Serial.printf("install: %s: HTTP %d, %lld bytes (of %llu)\n", h, status, (long long)len, (unsigned long long)g_fd_total);
      if (status == 206 && len > 0 && len <= cap) {
        int got = 0;
        while (got < len) {
          const int n = esp_http_client_read(c, (char*)buf + got, len - got);
          if (n <= 0) break;
          got += n;
        }
        if (got == len) return got;   // (all of it read: the connection can go on to the next)
      }
    }
    esp_http_client_close(c);
    vTaskDelay(pdMS_TO_TICKS(500 << attempt));
  }
  return -1;
}

static uint32_t fd_crc32(uint32_t crc, const uint8_t* p, size_t n) {   // the zip's CRC-32
  static uint32_t table[256];
  if (!table[1])
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
  crc = ~crc;
  while (n--) crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
  return ~crc;
}

static inline uint32_t le16(const uint8_t* p) { return p[0] | p[1] << 8; }
static inline uint32_t le32(const uint8_t* p) { return le16(p) | le16(p + 2) << 16; }

// mbedTLS's memory from PSRAM while downloading: in internal RAM (as the Arduino core has it) a TLS session's ~40 KB
// took all the DMA-capable memory Wi-Fi leaves (~29 KB), and the AES unit's DMA descriptors couldn't be had
static void* fd_tls_calloc(size_t n, size_t size) { return heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM); }

// Download the zip's FD14LITE.img to FD_SETUP; false (and g_fd_msg) if that failed
static bool fd_download(uint8_t* buf, uint8_t* dict, tinfl_decompressor* inf);
static bool fd_download_psram_tls(uint8_t* buf, uint8_t* dict, tinfl_decompressor* inf) {
  mbedtls_platform_set_calloc_free(fd_tls_calloc, free);
  const bool ok = fd_download(buf, dict, inf);
  mbedtls_platform_set_calloc_free(fd_idf_tls_calloc, fd_idf_tls_free);
  return ok;
}
static bool fd_download(uint8_t* buf, uint8_t* dict, tinfl_decompressor* inf) {
  Serial.printf("install: DMA-capable internal RAM %u free (largest %u), internal %u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  esp_http_client_config_t cfg = {};
  cfg.url = FD_URL;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.keep_alive_enable = true;
  cfg.timeout_ms = 20000;
  cfg.event_handler = fd_http_event;
  cfg.buffer_size = 4096;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) { fd_say("no memory for the download"); return false; }
  bool ok = false;
  FILE* out = nullptr;
  do {
    // the zip's central directory, at its end: where FD14LITE.img is, how long, its CRC
    fd_say("Connecting to freedos.org ...");
    g_fd_total = 0;
    int tail = fd_get(c, "-16384", buf, FD_CHUNK);   // (the central directory: in the last 16 KB, else 64 KB)
    for (int i = tail - 22; i >= 0 && tail == 16384; --i)
      if (le32(buf + i) == 0x06054b50) {
        if (le32(buf + i + 16) < g_fd_total - tail) tail = fd_get(c, "-65536", buf, FD_CHUNK);
        break;
      }
    if (tail < 22 || !g_fd_total) { fd_say("Could not reach download.freedos.org"); break; }
    int eocd = -1;
    for (int i = tail - 22; i >= 0 && eocd < 0; --i)
      if (le32(buf + i) == 0x06054b50) eocd = i;
    const uint64_t tail0 = g_fd_total - tail;
    const uint32_t cd_off = eocd >= 0 ? le32(buf + eocd + 16) : 0, cd_n = eocd >= 0 ? le16(buf + eocd + 10) : 0;
    uint32_t method = 0, crc = 0, csize = 0, usize = 0, lho = 0;
    bool found = false;
    for (uint64_t p = cd_off; eocd >= 0 && cd_n && p >= tail0 && p + 46 <= g_fd_total && !found;) {
      const uint8_t* e = buf + (p - tail0);
      if (le32(e) != 0x02014b50) break;
      const uint32_t nlen = le16(e + 28), xlen = le16(e + 30), clen = le16(e + 32);
      if (nlen == strlen(FD_ENTRY) && !strncasecmp((const char*)e + 46, FD_ENTRY, nlen)) {
        method = le16(e + 10), crc = le32(e + 16), csize = le32(e + 20), usize = le32(e + 24), lho = le32(e + 42);
        found = true;
      }
      p += 46 + nlen + xlen + clen;
    }
    if (!found || method != 8) { fd_say("FreeDOS's download isn't as expected (%s)", FD_ENTRY); break; }
    char r[48];
    snprintf(r, sizeof(r), "%u-%u", (unsigned)lho, (unsigned)lho + 29);
    if (fd_get(c, r, buf, FD_CHUNK) != 30 || le32(buf) != 0x04034b50) { fd_say("Download failed (header)"); break; }
    const uint64_t data0 = lho + 30 + le16(buf + 26) + le16(buf + 28);

    // the image, in one piece on the card (raw access: disk_access.h), written as it is unzipped
    unlink(FD_SETUP);
    if (esp_vfs_fat_create_contiguous_file("/sdcard", FD_SETUP, usize, false) != ESP_OK ||
        !(out = fopen(FD_SETUP, "r+b"))) {
      fd_say("Can't make FDSETUP.IMG (card full?)");
      break;
    }
    tinfl_init(inf);
    size_t dict_ofs = 0;
    uint32_t got_crc = 0, written = 0;
    bool done = false, bad = false;
    for (uint64_t at = 0; at < csize && !done && !bad;) {
      const uint32_t n = (uint32_t)std::min<uint64_t>(FD_PIECE, csize - at);
      snprintf(r, sizeof(r), "%llu-%llu", (unsigned long long)(data0 + at), (unsigned long long)(data0 + at + n - 1));
      if (fd_get(c, r, buf, FD_CHUNK) != (int)n) { fd_say("Download failed: the connection was lost"); bad = true; break; }
      at += n;
      const int pct = (int)(at * 100 / csize);
      if (pct != g_fd_pct || at == csize) {   // (said when the percentage changes)
        g_fd_pct = pct;
        fd_say("Downloading FreeDOS 1.4: %d%%  (%u of %u KB)", pct, (unsigned)(at >> 10), (unsigned)(csize >> 10));
      }
      size_t in_ofs = 0;
      for (;;) {   // unzip what came (the ring buffer is the dictionary: 32 KB)
        size_t in_n = n - in_ofs, out_n = TINFL_LZ_DICT_SIZE - dict_ofs;
        const tinfl_status st = tinfl_decompress(inf, buf + in_ofs, &in_n, dict, dict + dict_ofs, &out_n,
                                                 at < csize ? TINFL_FLAG_HAS_MORE_INPUT : 0);
        in_ofs += in_n;
        if (out_n) {
          got_crc = fd_crc32(got_crc, dict + dict_ofs, out_n);
          bool wrote;
          {
            JitHold hold;   // (tab5_386.ino: core 0's cache operations on PSRAM)
            esp_cache_msync(dict, TINFL_LZ_DICT_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);   // (the card's DMA reads PSRAM)
            wrote = fwrite(dict + dict_ofs, 1, out_n, out) == out_n;
          }
          if (!wrote) { fd_say("Writing to the card failed"); bad = true; break; }
          written += out_n;
          dict_ofs = (dict_ofs + out_n) & (TINFL_LZ_DICT_SIZE - 1);
        }
        if (st == TINFL_STATUS_DONE) { done = true; break; }
        if (st < 0) { fd_say("The download is damaged (unzip)"); bad = true; break; }
        if (st == TINFL_STATUS_NEEDS_MORE_INPUT) break;
      }
    }
    if (bad) break;
    if (!done || written != usize || got_crc != crc) { fd_say("The download is damaged (CRC)"); break; }
    ok = true;
  } while (false);
  if (out && fclose(out) != 0) ok = false;
  if (!ok) unlink(FD_SETUP);
  esp_http_client_cleanup(c);
  return ok;
}

// ---- the machine's configuration ---------------------------------------------------------------------------

static const char* FD_INI_INSTALL =
    "; Tab5 PC machine settings, written by the firmware: FreeDOS 1.4's installer is running.\r\n"
    "; FDSETUP.IMG is FreeDOS's LiteUSB disk (the installer), C.IMG the PC's own disk it installs to (drive D: while\r\n"
    "; installing), NOCD.ISO an empty CD-ROM drive the installer wants. When the PC restarts with FreeDOS on C.IMG,\r\n"
    "; the firmware starts it from C.IMG alone (and deletes FDSETUP.IMG and NOCD.ISO).\r\n"
    "[pc]\r\nhda = @/FDSETUP.IMG\r\nhdb = @/C.IMG\r\ncdc = @/NOCD.ISO\r\n"
    "mem_size = 16M\r\nvga_mem_size = 2M\r\nfill_cmos = 1\r\n"
    "[cpu]\r\ngen = 4\r\nfpu = 1\r\n"
    "[tab5]\r\ninstall = 1\r\n";

static const char* FD_INI_FINAL =
    "; Tab5 PC machine settings. /sdcard is the root of the card; disk images are files on it.\r\n"
    "; FreeDOS 1.4, installed on C.IMG by its own installer.\r\n"
    "[pc]\r\n"
    "; hard disks hda hdb hdc hdd, CD-ROMs cda cdb cdc cdd (an .iso), floppies fda fdb (1.44 MB etc. images)\r\n"
    "hda = @/C.IMG\r\n;hdb = /sdcard/D.IMG\r\n;fda = /sdcard/floppy.img\r\n"
    "; the CD-ROM drive's disc (an .iso; the tray beside the PC's picture changes it; without the line: empty)\r\n"
    ";cdc = /sdcard/cd.iso\r\n"
    "mem_size = 16M\r\nvga_mem_size = 2M\r\n"
    "; 1: the hard disks' geometry in the CMOS (some systems want it, others not)\r\nfill_cmos = 1\r\n"
    "[cpu]\r\n; 3/4/5/6 = 386/486/586/686\r\ngen = 4\r\n; 1 = x87 maths coprocessor\r\nfpu = 1\r\n";

// The installer CD images at the card's root (1 MB or more; not a machine's NOCD.ISO), into g_fd_isos
static void fd_find_isos() {
  g_fd_iso_count = 0;
  DIR* d = opendir("/sdcard");
  if (!d) return;
  while (struct dirent* e = readdir(d)) {
    const size_t n = strlen(e->d_name);
    if (n < 5 || strcasecmp(e->d_name + n - 4, ".iso") || !strcasecmp(e->d_name, "NOCD.ISO")) continue;
    const String path = "/sdcard/" + String(e->d_name);
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= (1 << 20) && g_fd_iso_count < FD_MAX_ISOS)
      g_fd_isos[g_fd_iso_count++] = e->d_name;
  }
  closedir(d);
  std::sort(g_fd_isos, g_fd_isos + g_fd_iso_count, [](const String& a, const String& b) {
    return strcasecmp(a.c_str(), b.c_str()) < 0;
  });
}

static const char* FD_INI_ISO =
    "; Tab5 PC machine settings, written by the firmware: the PC starts from the CD (%s) while C.IMG has no\r\n"
    "; files, and from C.IMG once something is installed on it (the BIOS tries the hard disk first). Without the CD\r\n"
    "; in the drive: delete the cdc line.\r\n"
    "[pc]\r\nhda = @/C.IMG\r\ncdc = /sdcard/%s\r\n"
    "mem_size = 16M\r\nvga_mem_size = 2M\r\nfill_cmos = 1\r\n"
    "[cpu]\r\ngen = 4\r\nfpu = 1\r\n"
    "; until a system is on C.IMG (the firmware deletes this then)\r\n"
    "[tab5]\r\ninstall = 2\r\n";

// text into the file at path, in pieces under a sector, unbuffered: FatFs copies each through its own sector buffer.
// (Given 512 bytes or more at a sector's start, FatFs hands the caller's memory to the card's DMA, which needs it
// 128-byte aligned in DMA-capable memory: FD_INI_FINAL, in flash, failed so, "sdmmc_write_blocks failed (0x102)",
// and left tiny386.ini empty.)
static bool fd_write_file(const char* path, const char* text) {
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  setvbuf(f, nullptr, _IONBF, 0);
  bool ok = true;
  for (size_t at = 0, n = strlen(text); ok && at < n; at += 256)
    ok = fwrite(text + at, 1, std::min<size_t>(256, n - at), f) == std::min<size_t>(256, n - at);
  return fclose(f) == 0 && ok;
}

static int fd_ini_handler(void* user, const char* section, const char* name, const char* value) {
  if (!strcmp(section, "tab5") && !strcmp(name, "install")) *(int*)user = atoi(value);
  return 1;
}

// Is there a system (FreeDOS's KERNEL.SYS, or another DOS's IO.SYS) in the root of C.IMG's first FAT
// partition? Read from the card as it is (the PC writes C.IMG as raw card sectors: no copy of it waits in a buffer).
static bool fd_disk_has_system() {
  FILE* f = fopen(FD_DISK, "rb");
  if (!f) return false;
  static uint8_t s[512];
  auto sector = [&](uint64_t n) { return fseek(f, (long)(n * 512), SEEK_SET) == 0 && fread(s, 1, 512, f) == 512; };
  bool found = false;
  do {
    if (!sector(0) || s[510] != 0x55 || s[511] != 0xaa) break;
    uint32_t lba = 0;
    for (int i = 0; i < 4 && !lba; ++i)
      if (s[446 + 16 * i + 4]) lba = le32(s + 446 + 16 * i + 8);
    if (!lba || !sector(lba) || le16(s + 11) != 512) break;
    const uint32_t spc = s[13], reserved = le16(s + 14), fats = s[16], root_n = le16(s + 17);
    const uint32_t fatsz = le16(s + 22) ? le16(s + 22) : le32(s + 36), root_clus = le32(s + 44);
    if (!spc || !fats || !fatsz) break;
    uint64_t root = lba + reserved + (uint64_t)fats * fatsz, count = (root_n * 32 + 511) / 512;
    if (!root_n) {   // FAT32: the root directory's first cluster
      root += (uint64_t)(root_clus - 2) * spc;
      count = spc;
    }
    for (uint64_t k = 0; k < count && !found; ++k) {
      if (!sector(root + k)) break;
      for (int e = 0; e < 512 && !found; e += 32) {
        if (s[e] == 0) { k = count; break; }
        found = !memcmp(s + e, "KERNEL  SYS", 11) || !memcmp(s + e, "IO      SYS", 11);
      }
    }
  } while (false);
  fclose(f);
  return found;
}

// C:\FDAUTO.BAT as FreeDOS's installer wrote it, made right for this PC: BLASTER the Sound Blaster 16 it has (T6; the
// installer's P330 is an MPU-401, which it hasn't), and without a CD-ROM drive (!cd) CDROM.BAT, the CD-ROM driver,
// left out (else "CD-ROM not configured", after the driver's search, at every start). C.IMG is mounted as a disk
// session (disk_access.h) while the PC isn't running: before it starts, or from core 1 itself (its reset; the Tab5
// restarts after, as the PC's disk may have parts of C.IMG read ahead).
static void fd_fix_startup(bool cd) {
  FILE* img = fopen(FD_DISK, "r+b");
  if (!img) return;
  String err, text, out;
  const char* mount = disk_mount(img, 0, err);
  if (!mount) Serial.printf("install: C.IMG: %s\n", err.c_str());
  const String path = String(mount ? mount : "") + "/FDAUTO.BAT";
  if (FILE* f = mount ? fopen(path.c_str(), "rb") : nullptr) {
    char buf[256];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.concat(buf, n);
    fclose(f);
  }
  int changed = 0;
  for (int at = 0; at < (int)text.length();) {
    const int nl = text.indexOf('\n', at), end = nl < 0 ? text.length() : nl + 1;
    String line = text.substring(at, end), l = line;
    at = end;
    l.trim();
    l.toLowerCase();
    if (!cd && l.startsWith("call %dosdir%\\bin\\cdrom.bat")) {
      line = "rem (no CD-ROM drive: with a cdc line in tiny386.ini, delete this rem) " + line;
      ++changed;
    } else if (l.startsWith("set blaster=") && l != "set blaster=a220 i5 d1 h5 t6") {
      line = "set BLASTER=A220 I5 D1 H5 T6\r\n";
      ++changed;
    }
    out += line;
  }
  if (changed) {
    if (!fd_write_file(path.c_str(), out.c_str())) Serial.println("install: writing C:\\FDAUTO.BAT failed");
    else Serial.printf("install: C:\\FDAUTO.BAT: %d lines changed (CD-ROM, BLASTER)\n", changed);
  }
  if (mount) {
    disk_unmount();
    g_disk = DiskSession();
  }
  fclose(img);
}

// The install over (a system on C.IMG): FreeDOS's installer's disks go and the PC starts from C.IMG alone; or, from a
// CD, the install lines go (the CD stays in the drive). True: the configuration changed (the Tab5 restarts, with the
// translator on).
static bool fd_install_finish() {
  if (!g_install_mode || !fd_disk_has_system()) return false;
  if (g_install_mode == 2) {
    fd_fix_startup(true);
    String ini;
    if (FILE* f = fopen(FD_INI, "rb")) {
      char buf[256];
      for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) ini.concat(buf, n);
      fclose(f);
    }
    for (const char* line : {"; until a system", "[tab5]", "install = 2"}) {   // (the ini's last lines)
      const int at = ini.indexOf(line), nl = at < 0 ? -1 : ini.indexOf('\n', at);
      if (at >= 0) ini.remove(at, nl < 0 ? ini.length() - at : nl + 1 - at);
    }
    if (!fd_write_file(FD_INI, ini.c_str())) return false;
    g_install_mode = 0;
    Serial.println("install: a system is on C.IMG");
    return true;
  }
  fd_fix_startup(true);   // (every machine has a CD-ROM drive: cd_drive.h)
  if (!fd_write_file(FD_INI, fd_ini_text(FD_INI_FINAL).c_str())) return false;
  unlink(FD_SETUP);
  unlink(FD_NOCD);
  g_install_mode = 0;
  Serial.println("install: FreeDOS is on C.IMG; starting from it");
  return true;
}

// ---- the install, a task on core 0 ----------------------------------------------------------------------------

// The first sector of a new C.IMG: our boot code (it starts the active partition, or the next boot device while that
// has no files: an installer's CD) and one active FAT16 partition, in whole cylinders of tiny386's geometry (16
// heads, 63 sectors): DOS sees the drive at once, and installers skip partitioning (and its restart). At most 1024
// cylinders (504 MB), where CHS reaches: a type 06 partition beyond them makes FreeDOS's kernel ask for FDISK at
// every start, and older DOSes don't know the LBA type (0E). Returns the partition's size in sectors (it starts at 63).
static uint32_t fd_mbr(uint8_t* m, uint64_t bytes) {
  memset(m, 0, 512);
  memcpy(m, MBR_CODE, sizeof(MBR_CODE));
  const uint32_t H = 16, S = 63, total = (uint32_t)(bytes / 512), start = S;
  const uint32_t cyls = total / (H * S) < 1024 ? total / (H * S) : 1024;
  const uint32_t count = cyls * H * S - start;
  auto chs = [&](uint32_t lba, uint8_t* p) {
    uint32_t c = lba / (H * S), h = lba / S % H, sec = lba % S + 1;
    if (c > 1023) c = 1023, h = H - 1, sec = S;
    p[0] = h;
    p[1] = ((c >> 2) & 0xc0) | sec;
    p[2] = c & 0xff;
  };
  uint8_t* e = m + 446;
  e[0] = 0x80;   // active
  chs(start, e + 1);
  e[4] = 0x06;   // FAT16 (over 32 MB)
  chs(start + count - 1, e + 5);
  memcpy(e + 8, &start, 4);
  memcpy(e + 12, &count, 4);
  m[510] = 0x55;
  m[511] = 0xaa;
  return count;
}

// The partition (`count` sectors from `start`), formatted FAT16 and empty as DOS's FORMAT leaves it, into p (its first
// sector; the FATs follow in the same blank buffer): 8 KB clusters, 512 root entries. Installers then find a drive
// ready: a DOS installer CD tested asked "Abort, Retry, Fail?" about an unformatted C:, then, formatting it, for
// its first setup disk again and again (in QEMU too). The boot sector's code is INT 18h, the next boot device
// (our MBR goes there itself while the root directory is empty); a system's SYS writes its own.
static void fd_format(uint8_t* p, uint32_t start, uint32_t count) {
  const uint32_t spc = 16, reserved = 1, root_n = 512, root_secs = root_n * 32 / 512;
  const uint32_t fatsz = (count - reserved - root_secs + 256 * spc + 2 - 1) / (256 * spc + 2);   // (the FAT specification's)
  const uint32_t serial = esp_random();
  uint8_t* b = p;
  static const uint8_t JMP[3] = {0xeb, 0x3c, 0x90};
  memcpy(b, JMP, 3);
  memcpy(b + 3, "TAB5-PC ", 8);   // (the OEM name: any)
  b[12] = 2;   // 512 bytes a sector
  b[13] = spc;
  b[14] = reserved;
  b[16] = 2;   // FATs
  b[17] = root_n & 0xff, b[18] = root_n >> 8;
  b[21] = 0xf8;   // a hard disk
  b[22] = fatsz & 0xff, b[23] = fatsz >> 8;
  b[24] = 63, b[26] = 16;   // sectors a track, heads
  memcpy(b + 28, &start, 4);   // hidden sectors
  memcpy(b + 32, &count, 4);   // (over 65535: the 32-bit count)
  b[36] = 0x80;
  b[38] = 0x29;
  memcpy(b + 39, &serial, 4);
  memcpy(b + 43, "NO NAME    FAT16   ", 19);
  static const uint8_t CODE[4] = {0xcd, 0x18, 0xeb, 0xfe};   // int 18h; jmp $
  memcpy(b + 62, CODE, 4);
  b[510] = 0x55, b[511] = 0xaa;
  for (uint32_t i = 0; i < 2; ++i) {   // each FAT: the media byte, then the end-of-chain mark
    uint8_t* fat = p + (reserved + i * fatsz) * 512;
    fat[0] = 0xf8, fat[1] = fat[2] = fat[3] = 0xff;
  }
}

// C.IMG, 512 MB in one piece: partitioned (fd_mbr) and formatted (fd_format), the rest of its first MB blank
static bool fd_make_disk() {
  fd_say("Making the PC's disk (C.IMG, 512 MB) ...");
  uint8_t* buf = (uint8_t*)heap_caps_aligned_alloc(128, FD_DISK_HEAD, MALLOC_CAP_SPIRAM);   // (a PC running: its own)
  if (!buf) { fd_say("Not enough memory to make C.IMG"); return false; }
  if (esp_vfs_fat_create_contiguous_file("/sdcard", FD_DISK, FD_DISK_SIZE, true) != ESP_OK) {
    fd_say("Can't make C.IMG: the card needs 550 MB free");
    return false;
  }
  FILE* f = fopen(FD_DISK, "r+b");
  bool ok = f != nullptr;
  memset(buf, 0, FD_DISK_HEAD);
  fd_format(buf + 63 * 512, 63, fd_mbr(buf, FD_DISK_SIZE));   // (within FD_DISK_HEAD: up to sector 63 + 1 + 252)
  JitHold hold;   // (tab5_386.ino: core 0's cache operations on PSRAM)
  esp_cache_msync(buf, FD_DISK_HEAD, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  for (int at = 0; ok && at < (1 << 20);) {   // the first MB: the data, then zeros
    const int n = std::min(FD_DISK_HEAD, (1 << 20) - at);
    ok = fwrite(buf, 1, n, f) == (size_t)n;
    if (at == 0) {
      memset(buf, 0, FD_DISK_HEAD);
      esp_cache_msync(buf, FD_DISK_HEAD, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
    at += n;
  }
  if (f && fclose(f) != 0) ok = false;
  free(buf);
  if (!ok) {
    fd_say("Writing to the card failed");
    unlink(FD_DISK);
  }
  return ok;
}

static bool fd_install_run() {
  uint8_t* buf = (uint8_t*)heap_caps_aligned_alloc(128, FD_CHUNK, MALLOC_CAP_SPIRAM);
  uint8_t* dict = (uint8_t*)heap_caps_aligned_calloc(128, 1, TINFL_LZ_DICT_SIZE, MALLOC_CAP_SPIRAM);
  tinfl_decompressor* inf = (tinfl_decompressor*)heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
  bool ok = false;
  do {
    if (!buf || !dict || !inf) { fd_say("Not enough memory"); break; }
    struct stat st;
    if (stat(FD_DISK, &st) == 0) { fd_say("%s/C.IMG is already on the card: delete it first (it isn't replaced)", g_fd_dir.c_str() + 7); break; }
    mkdir(g_fd_dir.c_str(), 0777);
    if (g_fd_iso.length()) {   // the CD on the card: a blank C: drive, and the PC starts from the CD
      String ini = FD_INI_ISO;
      ini.replace("%s", g_fd_iso);
      if (!fd_make_disk()) break;
      if (!fd_write_file(FD_INI, fd_ini_text(ini.c_str()).c_str())) { fd_say("Writing to the card failed"); unlink(FD_DISK); break; }
      ok = true;
      break;
    }
    if (!fd_download_psram_tls(buf, dict, inf)) break;
    if (!fd_make_disk()) { unlink(FD_SETUP); break; }
    if (!fd_write_file(FD_NOCD, "") || !fd_write_file(FD_INI, fd_ini_text(FD_INI_INSTALL).c_str())) {
      fd_say("Writing to the card failed");
      unlink(FD_DISK);
      unlink(FD_SETUP);
      break;
    }
    ok = true;
  } while (false);
  free(buf);
  free(dict);
  free(inf);
  return ok;
}

// The task, made at the first install and waiting for the next after a failure. Its stack is in PSRAM: in internal
// RAM its 24 KB (TLS needs room) took the DMA-capable memory Wi-Fi leaves, and the AES unit's DMA descriptors
// couldn't be had. (It never writes to flash, which a task with its stack in PSRAM mustn't.)
static TaskHandle_t g_fd_task = nullptr;
static void fd_install_task(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!fd_install_run()) {
      g_fd_state = FD_FAILED;
      continue;
    }
    // (install_mode_run remembers the machine and restarts; not from here, a task with its stack in PSRAM, which
    // mustn't write to flash: NVS)
    g_fd_state = FD_IDLE;
    fd_say(g_fd_iso.length() ? "Done. Restarting from the CD ..." : "Done. Restarting into FreeDOS's installer ...");
  }
}

// The install of the machine named name (in install mode): FreeDOS's download, or from the CD image iso at the card's
// root
static void fd_install_start(const char* name, const char* iso = "") {
  if (g_fd_state == FD_RUNNING) return;
  g_fd_iso = iso;
  fd_use("/sdcard/tiny386-" + String(name) + ".ini");
  if (!g_fd_task && xTaskCreatePinnedToCoreWithCaps(fd_install_task, "fdinstall", 24576, nullptr, 1, &g_fd_task, 0,
                                                    MALLOC_CAP_SPIRAM) != pdPASS) {
    g_fd_task = nullptr;
    g_fd_state = FD_FAILED;
    fd_say("Not enough memory to start");
    return;
  }
  g_fd_state = FD_RUNNING;
  g_fd_pct = 0;
  fd_say("Starting ...");
  xTaskNotifyGive(g_fd_task);
}
