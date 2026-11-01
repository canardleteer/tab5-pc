// tiny386 core: ide.c (see tab5_config.h)
#include "tab5_config.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "sdmmc_cmd.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "t386_raw.h"

// Disk images unbuffered: the guest's writes reach the card at once (not lost if the Tab5 is reset), and the web
// server's file transfers (disk_access.h), which borrow the same FILE while the PC is paused, see no stale buffer.
static FILE* t386_fopen(const char* path, const char* mode) {
    FILE* f = fopen(path, mode);
    if (f) setvbuf(f, NULL, _IONBF, 0);
    return f;
}

// Raw access: a disk image is read and written as card sectors, bypassing FatFs, through a list of where its pieces
// lie on the card (made once at startup, see image_runs(): one piece for an image written to a fresh card, more if
// the card was in use). Through FatFs every backward seek in a big file walks its cluster chain from the start (~125
// FAT sector reads for 500 MB), and DOS seeks back and forth between its FAT and its data all the time. Transfers go
// through a DMA buffer (whole sectors, so it can be in PSRAM), so the SDMMC driver can do many sectors per command;
// one that runs on into the next piece is split there.
typedef struct {
    FILE* f;
    sdmmc_card_t* card;
    T386Run* runs;               // in image order, together covering the image
    uint32_t nruns, sectors;     // ... and its size (512-byte sectors)
    int64_t pos;
} T386Raw;
static T386Raw t386_raw[4];
static SemaphoreHandle_t t386_raw_lock;
static uint8_t* t386_bounce;
#define T386_BOUNCE_SECTORS 64

// Read-ahead: tiny386's IDE reads a sector at a time (READ SECTORS, what the BIOS and DOS use), and each was a command
// to the card. A read of a few sectors that misses takes 8 (a FAT16 cluster of 4 KB), or 32 if it goes on from where
// the last one ended, into this buffer; the following ones come from it. Writes there empty it.
#define T386_RA_SECTORS 32
static uint8_t* t386_ra;
static T386Raw* t386_ra_of;
static uint32_t t386_ra_sector, t386_ra_count, t386_ra_next;   // (t386_ra_next: where the last read ended)

// One command to the card at a time. Raw access (core 1) and FatFs (core 0: the web server, the settings panel; or
// the PC's images through FatFs) each had their own lock, and a read sent while the card was still busy with the
// other's write timed out (0x107): downloads cut short, and the guest would see I/O errors on its disk. So FatFs's
// driver for the card is replaced by this one, the same as ESP-IDF's (diskio_sdmmc.c) but taking t386_card_lock, as
// t386_card_rw does.
static SemaphoreHandle_t t386_card_lock;
static sdmmc_card_t* t386_ff_card;
// FatFs's own sectors (its window, a file's buffer: in structures, not on cache lines) go through this, whole cache
// lines in PSRAM, which the card's DMA takes. Left to the card's driver, each took a sector of DMA-capable internal
// RAM for the moment, of which Wi-Fi leaves ~3 KB: with traffic on the web page that ran out, and the card's reads
// failed ("not enough mem": a file not created, a folder listed empty, the card as if gone).
#define T386_FF_BOUNCE_SECTORS 16
static uint8_t* t386_ff_bounce;

static DSTATUS t386_ff_init(BYTE pdrv) { return 0; }
static DSTATUS t386_ff_status(BYTE pdrv) { return 0; }

static DRESULT t386_ff_read(BYTE pdrv, BYTE* buff, uint32_t sector, unsigned count) {
    xSemaphoreTake(t386_card_lock, portMAX_DELAY);
    esp_err_t e = ESP_OK;
    if (!((uintptr_t)buff & 127) || !t386_ff_bounce) {
        e = sdmmc_read_sectors(t386_ff_card, buff, sector, count);
    } else {
        for (unsigned done = 0; done < count && e == ESP_OK;) {
            const unsigned n = count - done < T386_FF_BOUNCE_SECTORS ? count - done : T386_FF_BOUNCE_SECTORS;
            e = sdmmc_read_sectors(t386_ff_card, t386_ff_bounce, sector + done, n);
            if (e == ESP_OK) memcpy(buff + done * 512, t386_ff_bounce, n * 512);
            done += n;
        }
    }
    xSemaphoreGive(t386_card_lock);
    return e == ESP_OK ? RES_OK : RES_ERROR;
}

static DRESULT t386_ff_write(BYTE pdrv, const BYTE* buff, uint32_t sector, unsigned count) {
    xSemaphoreTake(t386_card_lock, portMAX_DELAY);
    esp_err_t e = ESP_OK;
    if (!((uintptr_t)buff & 127) || !t386_ff_bounce) {
        e = sdmmc_write_sectors(t386_ff_card, buff, sector, count);
    } else {
        for (unsigned done = 0; done < count && e == ESP_OK;) {
            const unsigned n = count - done < T386_FF_BOUNCE_SECTORS ? count - done : T386_FF_BOUNCE_SECTORS;
            memcpy(t386_ff_bounce, buff + done * 512, n * 512);
            esp_cache_msync(t386_ff_bounce, n * 512, ESP_CACHE_MSYNC_FLAG_DIR_C2M);   // (the card's DMA reads PSRAM)
            e = sdmmc_write_sectors(t386_ff_card, t386_ff_bounce, sector + done, n);
            done += n;
        }
    }
    xSemaphoreGive(t386_card_lock);
    return e == ESP_OK ? RES_OK : RES_ERROR;
}

static DRESULT t386_ff_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    switch (cmd) {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_COUNT: *(DWORD*)buff = t386_ff_card->csd.capacity; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD*)buff = t386_ff_card->csd.sector_size; return RES_OK;
    }
    return RES_ERROR;   // (GET_BLOCK_SIZE as ESP-IDF's; no TRIM: FatFs doesn't need it)
}

// Once the card is mounted (disk_access.h)
void t386_card_lock_setup(void* card) {
    static const ff_diskio_impl_t impl = {
        .init = t386_ff_init, .status = t386_ff_status, .read = t386_ff_read, .write = t386_ff_write,
        .ioctl = t386_ff_ioctl,
    };
    if (t386_card_lock) return;
    t386_card_lock = xSemaphoreCreateMutex();
    t386_ff_card = card;
    t386_ff_bounce = heap_caps_aligned_alloc(128, T386_FF_BOUNCE_SECTORS * 512, MALLOC_CAP_SPIRAM);
    ff_diskio_register(ff_diskio_get_pdrv_card(card), &impl);
}

// (serial "S": the card's commands, the sectors moved and the time they took, the reads read-ahead served, and the
// commands tried again)
uint32_t t386_disk_cmds, t386_disk_sectors, t386_disk_hits, t386_disk_retries;
uint64_t t386_disk_us;
// A command that fails is tried again (it shouldn't: the lock above), so that the guest doesn't see an I/O error
static int t386_card_rw(sdmmc_card_t* card, int write, uint8_t* buf, uint32_t sector, uint32_t n) {
    const int64_t t0 = esp_timer_get_time();
    esp_err_t e = ESP_FAIL;
    for (int tries = 0; tries < 4 && e != ESP_OK; ++tries) {
        if (tries) {
            t386_disk_retries++;
            vTaskDelay(1);
        }
        xSemaphoreTake(t386_card_lock, portMAX_DELAY);
        e = write ? sdmmc_write_sectors(card, buf, sector, n) : sdmmc_read_sectors(card, buf, sector, n);
        xSemaphoreGive(t386_card_lock);
    }
    t386_disk_us += esp_timer_get_time() - t0;
    t386_disk_cmds++;
    t386_disk_sectors += n;
    return e != ESP_OK;
}

static T386Raw* t386_raw_of(FILE* f) {
    for (int i = 0; i < 4; ++i)
        if (f && t386_raw[i].f == f) return &t386_raw[i];
    return NULL;
}

// The card sector of image sector `sector` (< r->sectors); *n: how many follow it on the card, up to the piece's end.
static uint32_t t386_raw_map(const T386Raw* r, uint32_t sector, uint32_t* n) {
    uint32_t lo = 0, hi = r->nruns - 1;
    while (lo < hi) {   // the last run starting at or before sector
        const uint32_t mid = (lo + hi + 1) / 2;
        if (r->runs[mid].sector <= sector) lo = mid; else hi = mid - 1;
    }
    const T386Run* run = &r->runs[lo];
    *n = run->count - (sector - run->sector);
    return run->card + (sector - run->sector);
}

// Takes runs (heap_caps_malloc'ed; freed if it fails): nruns pieces, in image order, of an image of `sectors` sectors.
int t386_raw_attach(FILE* f, void* card, T386Run* runs, uint32_t nruns, uint32_t sectors) {
    if (!t386_raw_lock) {
        t386_raw_lock = xSemaphoreCreateMutex();
        // in PSRAM, whole cache lines (the SDMMC driver's DMA takes those, for whole sectors): internal RAM is what
        // WiFi runs short of
        t386_bounce = heap_caps_aligned_alloc(128, T386_BOUNCE_SECTORS * 512, MALLOC_CAP_SPIRAM);
        t386_ra = heap_caps_aligned_alloc(128, T386_RA_SECTORS * 512, MALLOC_CAP_SPIRAM);
        if (!t386_bounce || !t386_ra) { heap_caps_free(runs); return 0; }
    }
    t386_ra_of = NULL;
    for (int i = 0; i < 4; ++i)
        if (!t386_raw[i].f) {
            t386_raw[i] = (T386Raw){f, (sdmmc_card_t*)card, runs, nruns, sectors, 0};
            return 1;
        }
    heap_caps_free(runs);
    return 0;
}

// Read or write `count` sectors of the image at `sector`; 0 on success. Falls back to stdio if f isn't raw.
int t386_raw_rw(FILE* f, int write, void* buf, uint32_t sector, uint32_t count) {
    T386Raw* r = t386_raw_of(f);
    if (!r) {
        if (fseeko(f, (off_t)sector * 512, SEEK_SET) != 0) return -1;
        return (write ? fwrite(buf, 512, count, f) : fread(buf, 512, count, f)) == count ? 0 : -1;
    }
    if (sector + count > r->sectors) return -1;
    xSemaphoreTake(t386_raw_lock, portMAX_DELAY);
    int err = 0;
    if (!write && count <= 8) {   // (read-ahead, above)
        if (t386_ra_of != r || sector < t386_ra_sector || sector + count > t386_ra_sector + t386_ra_count) {
            uint32_t n = sector == t386_ra_next ? T386_RA_SECTORS : 8, run;
            if (n > r->sectors - sector) n = r->sectors - sector;
            const uint32_t cs = t386_raw_map(r, sector, &run);
            if (n > run) n = run < count ? count : run;   // to the piece's end, but at least the sectors asked for
            t386_ra_of = NULL;
            if (n <= run) {
                err = t386_card_rw(r->card, 0, t386_ra, cs, n);
            } else {   // (those run on into the next piece: a read for each)
                for (uint32_t done = 0; done < n && !err; done += run) {
                    const uint32_t c = t386_raw_map(r, sector + done, &run);
                    if (run > n - done) run = n - done;
                    err = t386_card_rw(r->card, 0, t386_ra + done * 512, c, run);
                }
            }
            if (!err) t386_ra_of = r, t386_ra_sector = sector, t386_ra_count = n;
        } else {
            t386_disk_hits++;
        }
        if (!err) memcpy(buf, t386_ra + (sector - t386_ra_sector) * 512, count * 512);
        t386_ra_next = sector + count;
        xSemaphoreGive(t386_raw_lock);
        return err ? -1 : 0;
    }
    if (write && t386_ra_of == r && sector < t386_ra_sector + t386_ra_count && sector + count > t386_ra_sector)
        t386_ra_of = NULL;
    for (uint32_t done = 0; done < count && !err;) {
        uint32_t n = count - done < T386_BOUNCE_SECTORS ? count - done : T386_BOUNCE_SECTORS, run;
        const uint32_t cs = t386_raw_map(r, sector + done, &run);
        if (n > run) n = run;
        uint8_t* p = (uint8_t*)buf + done * 512;
        if (write) {
            memcpy(t386_bounce, p, n * 512);
            esp_cache_msync(t386_bounce, n * 512, ESP_CACHE_MSYNC_FLAG_DIR_C2M);   // (the card's DMA reads PSRAM)
            err = t386_card_rw(r->card, 1, t386_bounce, cs, n);
        } else {
            err = t386_card_rw(r->card, 0, t386_bounce, cs, n);
            if (!err) memcpy(p, t386_bounce, n * 512);
        }
        done += n;
    }
    xSemaphoreGive(t386_raw_lock);
    return err ? -1 : 0;
}

static int t386_fseeko(FILE* f, off_t off, int whence) {
    T386Raw* r = t386_raw_of(f);
    if (r && whence == SEEK_SET) { r->pos = off; return 0; }
    return fseeko(f, off, whence);
}

static size_t t386_fread(void* buf, size_t size, size_t n, FILE* f) {
    T386Raw* r = t386_raw_of(f);
    const size_t bytes = size * n;
    if (!r || r->pos % 512 || bytes % 512) return fread(buf, size, n, f);
    if (t386_raw_rw(f, 0, buf, r->pos / 512, bytes / 512) != 0) return 0;
    r->pos += bytes;
    return n;
}

static size_t t386_fwrite(const void* buf, size_t size, size_t n, FILE* f) {
    T386Raw* r = t386_raw_of(f);
    const size_t bytes = size * n;
    if (!r || r->pos % 512 || bytes % 512) return fwrite(buf, size, n, f);
    if (t386_raw_rw(f, 1, (void*)buf, r->pos / 512, bytes / 512) != 0) return 0;
    r->pos += bytes;
    return n;
}

#define fopen t386_fopen
#define fseeko t386_fseeko
#define fread t386_fread
#define fwrite t386_fwrite
#include "ide.c"
#undef fopen
#undef fseeko
#undef fread
#undef fwrite

// Tab5 addition: the image file behind a hard disk (drive 0/1 of an IDE interface), or NULL (none, or a CD-ROM).
FILE* t386_ide_image(IDEIFState* s, int drive) {
    IDEState* d = s->drives[drive];
    if (!d || d->drive_kind != IDE_HD || !d->bs || d->bs->read_async != bf_read_async)
        return NULL;
    return ((BlockDeviceFile*)d->bs->opaque)->f;
}
