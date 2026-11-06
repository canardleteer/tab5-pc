// The PC's hard disks, opened from the Tab5 itself (for the web server's file manager, web.h).
//
// A disk image's first FAT partition is mounted with the Tab5's own FatFs at /hda .. /hdd while the PC is paused.
// DOS keeps parts of its disks (FAT, directories) in memory, so a disk changed underneath it must not be used by the
// same DOS again: when the session ends, the PC restarts if anything was changed (and simply continues if the disk
// was only read). A session ends on request (web: "Done") or DISK_IDLE_MS after the last use.
//
// The image is read and written the way tiny386 does (t386_raw_rw: raw card sectors, piece by piece, else its own
// unbuffered FILE), so there is no second view of the file that could disagree with it.
#pragma once
#include <ff.h>
#include <diskio_impl.h>
#include <esp_vfs_fat.h>

extern "C" FILE* t386_ide_image(IDEIFState* s, int drive);
#include "src/t386_raw.h"
extern "C" int t386_ide_cd_attach(IDEIFState* s, int drive, const char* path);
extern "C" int (*t386_raw_runs_hook)(const char* path, T386Run** runs, uint32_t* nruns, uint32_t* sectors);

// ---- raw access to the images (t386_ide.c) --------------------------------------------------------------------

// The card behind SD_MMC (a protected member, reached through a derived class).
struct SDHandle : fs::SDMMCFS {
  static sdmmc_card_t* card() { return SD_MMC.*static_cast<sdmmc_card_t* fs::SDMMCFS::*>(&SDHandle::_card); }
  static uint8_t pdrv() { return SD_MMC.*static_cast<uint8_t fs::SDMMCFS::*>(&SDHandle::_pdrv); }
};

// Where a card file lies: its pieces on the card (*runs, heap_caps_malloc'ed, *nruns of them) and its size in
// sectors; false if it isn't a plain file of whole sectors on the card, or lies in more than MAX_RUNS pieces.
static constexpr uint32_t MAX_RUNS = 8192;   // (12 bytes each, in PSRAM)
static uint32_t g_runs_ms;   // how long the last image_runs took (serial "loads")
static bool image_runs(const char* path, T386Run*& runs, uint32_t& nruns, uint32_t& sectors) {
  const int64_t t0 = esp_timer_get_time();
  struct Took { int64_t t0; ~Took() { g_runs_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000); } } took{t0};
  runs = nullptr;
  nruns = 0;
  if (strncmp(path, "/sdcard/", 8) != 0) return false;
  char ffpath[300];
  snprintf(ffpath, sizeof(ffpath), "%u:%s", SDHandle::pdrv(), path + 7);
  FIL* fil = (FIL*)heap_caps_malloc(sizeof(FIL), MALLOC_CAP_INTERNAL);
  if (!fil) return false;
  bool ok = f_open(fil, ffpath, FA_READ) == FR_OK;
  if (ok) {
    FATFS* fs = fil->obj.fs;
#if FF_MAX_SS != FF_MIN_SS
    ok = fs->ssize == 512;
#endif
    const FSIZE_t size = f_size(fil), bcs = (FSIZE_t)fs->csize * 512;
    ok = ok && fil->obj.sclust >= 2 && size >= 512;
    sectors = (uint32_t)(size / 512);
    uint32_t cap = 0;
    // each cluster in turn (a seek forward goes on from the cluster before): after the one before it on the card,
    // or a new piece. The seek goes a sector into the cluster (onto it: FatFs leaves a seek to its very start on the
    // one before), on a sector's start, so that FatFs only follows the chain: one off a sector's start read that
    // sector into the file's buffer, a card command for every cluster (a 600 MB ISO took 13 s; the PC waits).
    const FSIZE_t into = fs->csize > 1 ? 512 : 1;
    for (FSIZE_t off = 0; ok && off < size; off += bcs) {
      DWORD clust = fil->obj.sclust;
      if (off) {
        ok = f_lseek(fil, off + into) == FR_OK;
        clust = fil->clust;
      }
      if (!ok || clust < 2) { ok = false; break; }
      const uint32_t card = (uint32_t)(fs->database + (LBA_t)(clust - 2) * fs->csize);
      const uint32_t sector = (uint32_t)(off / 512), count = sector + fs->csize < sectors ? fs->csize : sectors - sector;
      if (nruns && runs[nruns - 1].card + runs[nruns - 1].count == card) {
        runs[nruns - 1].count += count;
        continue;
      }
      if (nruns == cap) {
        cap = cap ? cap * 2 : 16;
        T386Run* more = cap <= MAX_RUNS ? (T386Run*)heap_caps_realloc(runs, cap * sizeof(T386Run), MALLOC_CAP_SPIRAM) : nullptr;
        if (!more) { ok = false; break; }
        runs = more;
      }
      runs[nruns++] = {sector, card, count};
    }
    f_close(fil);
  }
  free(fil);
  if (!ok) {
    heap_caps_free(runs);
    runs = nullptr;
  }
  return ok;
}

// At startup: raw access for every hard disk image on the card.
static void disk_raw_setup(const PCConfig& conf) {
  t386_card_lock_setup(SDHandle::card());
  if (!g_prefs.getBool("raw", true)) {   // serial "raw 0": for comparing
    Serial.println("disks: through FatFs (raw access off)");
    return;
  }
  // read-only images (CD discs, as they are put in: t386_fopen) too, if whole sectors (raw reads stop at the last)
  t386_raw_runs_hook = [](const char* path, T386Run** runs, uint32_t* nruns, uint32_t* sectors) -> int {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size % 512) return 0;
    return image_runs(path, *runs, *nruns, *sectors);
  };
  for (int i = 0; i < 4; ++i)   // the disc in the drive at the start, opened before this
    if (conf.iscd[i] && t386_ide_cd_attach(i < 2 ? g_pc->ide : g_pc->ide2, i & 1, conf.disks[i]))
      Serial.printf("cd%c: %s, raw access\n", 'a' + i, conf.disks[i]);
  for (int i = 0; i < 4; ++i) {
    FILE* f = t386_ide_image(i < 2 ? g_pc->ide : g_pc->ide2, i & 1);
    T386Run* runs;
    uint32_t nruns, n;
    if (!f) continue;
    if (image_runs(conf.disks[i], runs, nruns, n) && t386_raw_attach(f, SDHandle::card(), runs, nruns, n))
      Serial.printf("disk hd%c: %s at card sector %u%s, raw access (mapped in %u ms)\n", 'a' + i, conf.disks[i],
                    (unsigned)runs[0].card, nruns > 1 ? (String(", in ") + nruns + " pieces").c_str() : "",
                    (unsigned)g_runs_ms);
    else
      Serial.printf("disk hd%c: %s: through FatFs (slower)\n", 'a' + i, conf.disks[i]);
  }
}

static constexpr int64_t DISK_IDLE_MS = 30000;

struct DiskSession {
  int drive = -1;           // 0-3 = hda-hdd; -1: none open
  FILE* f = nullptr;
  uint32_t lba0 = 0, sectors = 0;
  BYTE pdrv = 0xFF;
  FATFS* fs = nullptr;
  bool changed = false;
  int64_t last_use = 0;
};
static DiskSession g_disk;
static SemaphoreHandle_t g_disk_lock;   // held by whoever uses the session (web handlers, the idle check)

static FILE* disk_image(int drive) {
  if (!g_pc || drive < 0 || drive > 3) return nullptr;
  return t386_ide_image(drive < 2 ? g_pc->ide : g_pc->ide2, drive & 1);
}

static DSTATUS dio_init(BYTE) { return 0; }
static DSTATUS dio_status(BYTE) { return 0; }
static DRESULT dio_read(BYTE, BYTE* buf, DWORD sector, UINT count) {
  if (sector + count > g_disk.sectors) return RES_PARERR;
  return t386_raw_rw(g_disk.f, 0, buf, g_disk.lba0 + sector, count) == 0 ? RES_OK : RES_ERROR;
}
static DRESULT dio_write(BYTE, const BYTE* buf, DWORD sector, UINT count) {
  if (sector + count > g_disk.sectors) return RES_PARERR;
  g_disk.changed = true;
  return t386_raw_rw(g_disk.f, 1, (void*)buf, g_disk.lba0 + sector, count) == 0 ? RES_OK : RES_ERROR;
}
static DRESULT dio_ioctl(BYTE, BYTE cmd, void* buf) {
  switch (cmd) {
    case CTRL_SYNC: fflush(g_disk.f); fsync(fileno(g_disk.f)); return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t*)buf = g_disk.sectors; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD*)buf = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD*)buf = 1; return RES_OK;
  }
  return RES_PARERR;
}
static const ff_diskio_impl_t DISK_IO = {dio_init, dio_status, dio_read, dio_write, dio_ioctl};

static void pc_hold(bool on);   // the .ino: pause the PC (counted) and wait until it has stopped

static void disk_unmount();

// Mount image f's first FAT partition at /hda .. /hdd (by `drive`), as the session (none open). Returns the mount
// point, or nullptr with err set.
static const char* disk_mount(FILE* f, int drive, String& err) {
  static char mount[8];
  auto fail = [&](const String& why) -> const char* {
    err = why;
    g_disk = DiskSession();
    return nullptr;
  };
  uint8_t mbr[512];
  if (t386_raw_rw(f, 0, mbr, 0, 1) != 0 || mbr[510] != 0x55 || mbr[511] != 0xAA) return fail("no partition table");
  uint32_t lba0 = 0, count = 0;
  for (int i = 0; i < 4 && !count; ++i) {   // the first FAT partition
    const uint8_t* e = mbr + 446 + 16 * i;
    if (e[4] == 0x01 || e[4] == 0x04 || e[4] == 0x06 || e[4] == 0x0B || e[4] == 0x0C || e[4] == 0x0E) {
      memcpy(&lba0, e + 8, 4);
      memcpy(&count, e + 12, 4);
    }
  }
  if (!count) return fail("no FAT partition");
  BYTE pdrv = 0xFF;
  if (ff_diskio_get_drive(&pdrv) != ESP_OK) return fail("no free FatFs drive");
  g_disk = DiskSession();
  g_disk.f = f;
  g_disk.lba0 = lba0;
  g_disk.sectors = count;
  g_disk.pdrv = pdrv;
  ff_diskio_register(pdrv, &DISK_IO);
  snprintf(mount, sizeof(mount), "/hd%c", 'a' + drive);
  char drv[3] = {(char)('0' + pdrv), ':', 0};
  esp_vfs_fat_conf_t conf = {mount, drv, 8};
  if (esp_vfs_fat_register_cfg(&conf, &g_disk.fs) != ESP_OK) {
    ff_diskio_unregister(pdrv);
    return fail("VFS register failed");
  }
  const FRESULT fr = f_mount(g_disk.fs, drv, 1);
  if (fr != FR_OK) {
    esp_vfs_fat_unregister_path(mount);
    ff_diskio_unregister(pdrv);
    return fail("mount failed (FatFs " + String((int)fr) + ")");
  }
  g_disk.drive = drive;
  g_disk.last_use = esp_timer_get_time() / 1000;
  return mount;
}

// Open hard disk `drive` (with g_disk_lock held). Returns its mount point, or nullptr with err set. Another open
// disk is closed first; the PC stays paused, and whether anything changed carries over.
static const char* disk_open(int drive, String& err) {
  static char mount[8];
  if (g_disk.drive == drive) {
    g_disk.last_use = esp_timer_get_time() / 1000;
    return mount;
  }
  FILE* f = disk_image(drive);
  if (!f) { err = "no hard disk image there"; return nullptr; }
  bool changed = false;
  if (g_disk.drive >= 0) {   // switching disks: the PC is already held
    changed = g_disk.changed;
    disk_unmount();
    g_disk = DiskSession();
  } else {
    pc_hold(true);
  }
  const char* m = disk_mount(f, drive, err);
  if (!m) {   // let the PC go again (restarted if a disk changed)
    if (changed) pc_cold_reset();
    pc_hold(false);
    return nullptr;
  }
  g_disk.changed = changed;
  strcpy(mount, m);
  Serial.printf("disk: hd%c open at %s (PC paused)\n", 'a' + drive, mount);
  return mount;
}

// Unmount the open disk (with g_disk_lock held), leaving the PC paused.
static void disk_unmount() {
  char drv[3] = {(char)('0' + g_disk.pdrv), ':', 0};
  char mount[8];
  snprintf(mount, sizeof(mount), "/hd%c", 'a' + g_disk.drive);
  f_mount(nullptr, drv, 0);
  esp_vfs_fat_unregister_path(mount);
  ff_diskio_unregister(g_disk.pdrv);
  fflush(g_disk.f);
  fsync(fileno(g_disk.f));
}

// Close the open disk (with g_disk_lock held): the PC continues, restarted if a disk changed.
static void disk_close() {
  if (g_disk.drive < 0) return;
  disk_unmount();
  const bool changed = g_disk.changed;
  Serial.printf("disk: hd%c closed, %s\n", 'a' + g_disk.drive, changed ? "changed: restarting the PC" : "unchanged");
  g_disk = DiskSession();
  if (changed) pc_cold_reset();
  pc_hold(false);
}

static void disk_setup() { g_disk_lock = xSemaphoreCreateMutex(); }

// From the core-0 task: end a session nobody has used for DISK_IDLE_MS.
static void disk_poll() {
  if (g_disk.drive < 0 || esp_timer_get_time() / 1000 - g_disk.last_use < DISK_IDLE_MS) return;
  if (xSemaphoreTake(g_disk_lock, 0) != pdTRUE) return;
  if (g_disk.drive >= 0 && esp_timer_get_time() / 1000 - g_disk.last_use >= DISK_IDLE_MS) disk_close();
  xSemaphoreGive(g_disk_lock);
}
