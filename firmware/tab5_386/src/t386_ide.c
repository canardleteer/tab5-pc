// tiny386 core: ide.c (see tab5_config.h)
#include "tab5_config.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "sdmmc_cmd.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "t386_raw.h"
#include <unistd.h>

// Card files are read and written through their descriptor, not stdio: ESP-IDF's newlib reads an unbuffered FILE a
// byte per call (0.05 MB/s from the card) and a buffered one through its small buffer (~1.1 MB/s, a 512-byte card
// command at a time), where read() hands FatFs the whole request (~10 MB/s). The FILE (unbuffered, t386_fopen) is
// only opened and closed.
static size_t t386_fd_io(FILE* f, int wr, void* buf, size_t bytes) {
    const int fd = fileno(f);
    size_t done = 0;
    while (done < bytes) {
        const ssize_t n = wr ? write(fd, (const char*)buf + done, bytes - done) : read(fd, (char*)buf + done, bytes - done);
        if (n <= 0) break;
        done += n;
    }
    return done;
}

static int t386_raw_attach_file(FILE* f, const char* path);

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
static T386Raw t386_raw[8];   // (hard disks, and CD images: the one in the drive, the next one while it changes)
static SemaphoreHandle_t t386_raw_lock;
static uint8_t* t386_bounce;
#define T386_BOUNCE_SECTORS 128   // (ide.c gathers a write command's blocks: up to 128 sectors at a time)

// Read-ahead: tiny386's IDE reads a sector at a time (READ SECTORS, what the BIOS and DOS use) or a block of up to 16
// (READ MULTIPLE, Linux), and each was a command to the card. A read of up to 16 sectors that misses takes 8 (a FAT16
// cluster of 4 KB) or as many as asked, or 64 if it goes on from where the last one ended, into this buffer; the
// following ones come from it. Writes there empty it.
#define T386_RA_SECTORS 64
#define T386_PF_SECTORS 128   // (the prefetch: up to a DMA command's piece, ide.c's WRITE_GATHER_SECTORS; and the
                              // read-ahead's buffer, the two swap: both grow to it at the first big read, so that
                              // a PC that makes none (DOS and its programs: the BIOS's PIO) keeps the memory for
                              // the code cache)
static uint32_t t386_buf_sectors = T386_RA_SECTORS;   // their size now
static uint8_t* t386_ra;
static T386Raw* t386_ra_of;
static uint32_t t386_ra_sector, t386_ra_count, t386_ra_next;   // (t386_ra_next: where the last read ended)
static uint32_t t386_big_next;   // where the last read of more than 16 sectors (DMA) ended

// Prefetch: while the guest goes through a sequential read, a task on core 0 reads the next T386_RA_SECTORS into a
// second buffer, which takes the read-ahead's place when the reads get there: the card works while the PC runs (it
// had waited for every read-ahead, ~20% of the time in a long Linux read). State under t386_raw_lock: IDLE, BUSY
// (the task reads; t386_pf_of/sector/count stay as they are until it's done), READY. A write that overlaps it, or
// closing its image, waits for a read under way and drops it.
enum { PF_IDLE, PF_BUSY, PF_READY };
static uint8_t* t386_pf;
static T386Raw* t386_pf_of;
static uint32_t t386_pf_sector, t386_pf_count;
static bool t386_ra_streak;   // the read-ahead's buffer came from a sequential read (or the prefetch): a prefetch then
                              // (a read that merely goes on from the one before, ext4's metadata say, doesn't start
                              // one: it would hold the card for nothing, ahead of the writes)
static volatile int t386_pf_state;
static TaskHandle_t t386_pf_task;
static SemaphoreHandle_t t386_pf_done;   // given when the task finishes a read
uint32_t t386_disk_prefetched;           // (serial "S": reads the prefetch served)

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
    for (int i = 0; i < 8; ++i)
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

// Read n sectors of image r from `sector` into dst (whole cache lines in PSRAM), a card command per piece they lie in
static int t386_raw_read_into(T386Raw* r, uint8_t* dst, uint32_t sector, uint32_t n) {
    int err = 0;
    for (uint32_t done = 0, run; done < n && !err; done += run) {
        const uint32_t c = t386_raw_map(r, sector + done, &run);
        if (run > n - done) run = n - done;
        err = t386_card_rw(r->card, 0, dst + done * 512, c, run);
    }
    return err;
}

static void t386_pf_main(void* arg) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const int err = t386_raw_read_into(t386_pf_of, t386_pf, t386_pf_sector, t386_pf_count);   // (BUSY: stable)
        xSemaphoreTake(t386_raw_lock, portMAX_DELAY);
        t386_pf_state = err ? PF_IDLE : PF_READY;
        xSemaphoreGive(t386_raw_lock);
        xSemaphoreGive(t386_pf_done);
    }
}

// With t386_raw_lock held: until no prefetch is under way (the lock let go meanwhile: the task takes it to finish)
static void t386_pf_wait(void) {
    while (t386_pf_state == PF_BUSY) {
        xSemaphoreGive(t386_raw_lock);
        xSemaphoreTake(t386_pf_done, portMAX_DELAY);
        xSemaphoreTake(t386_raw_lock, portMAX_DELAY);
    }
}

// With t386_raw_lock held: drop a prefetch of image r (NULL: any) that overlaps sectors [sector, sector + count)
static void t386_pf_drop(T386Raw* r, uint32_t sector, uint32_t count) {
    if (t386_pf_state == PF_IDLE || (r && t386_pf_of != r) ||
        (r && (sector >= t386_pf_sector + t386_pf_count || sector + count <= t386_pf_sector)))
        return;
    t386_pf_wait();
    t386_pf_state = PF_IDLE;
}

// With t386_raw_lock held: the read-ahead's and the prefetch's buffers T386_PF_SECTORS (once, for big reads; what they
// hold is dropped). Stays as it was if there is no memory.
static void t386_pf_grow(void) {
    if (t386_buf_sectors >= T386_PF_SECTORS) return;
    t386_pf_wait();
    uint8_t* a = heap_caps_aligned_alloc(128, T386_PF_SECTORS * 512, MALLOC_CAP_SPIRAM);
    uint8_t* b = heap_caps_aligned_alloc(128, T386_PF_SECTORS * 512, MALLOC_CAP_SPIRAM);
    if (!a || !b) {
        heap_caps_free(a);
        heap_caps_free(b);
        return;
    }
    heap_caps_free(t386_ra);
    heap_caps_free(t386_pf);
    t386_ra = a, t386_pf = b;
    t386_ra_of = NULL;
    t386_pf_state = PF_IDLE;
    t386_buf_sectors = T386_PF_SECTORS;
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
        // the prefetch: without it (no memory) reads wait for the card as before
        t386_pf = heap_caps_aligned_alloc(128, T386_RA_SECTORS * 512, MALLOC_CAP_SPIRAM);
        t386_pf_done = xSemaphoreCreateBinary();
        if (!t386_pf || !t386_pf_done ||
            xTaskCreatePinnedToCoreWithCaps(t386_pf_main, "prefetch", 4096, NULL, 3, &t386_pf_task, 0,
                                            MALLOC_CAP_SPIRAM) != pdPASS)
            t386_pf_task = NULL;
    }
    t386_ra_of = NULL;
    for (int i = 0; i < 8; ++i)
        if (t386_raw[i].f == f) {   // (already: a read-only image, t386_fopen)
            heap_caps_free(runs);
            return 1;
        }
    for (int i = 0; i < 8; ++i)
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
        if (lseek(fileno(f), (off_t)sector * 512, SEEK_SET) < 0) return -1;
        return t386_fd_io(f, write, buf, (size_t)count * 512) == (size_t)count * 512 ? 0 : -1;
    }
    if (sector + count > r->sectors) return -1;
    xSemaphoreTake(t386_raw_lock, portMAX_DELAY);
    int err = 0;
    if (!write && count <= 16) {   // (read-ahead, above; 16: Linux's READ MULTIPLE blocks)
        const bool sequential = sector == t386_ra_next;
        if (t386_ra_of != r || sector < t386_ra_sector || sector + count > t386_ra_sector + t386_ra_count) {
            // the prefetch has them (or is reading them): its buffer becomes the read-ahead's
            if (t386_pf_state != PF_IDLE && t386_pf_of == r && sector >= t386_pf_sector &&
                sector + count <= t386_pf_sector + t386_pf_count) {
                t386_pf_wait();
                if (t386_pf_state == PF_READY) {
                    uint8_t* b = t386_ra;
                    t386_ra = t386_pf, t386_pf = b;
                    t386_ra_of = r, t386_ra_sector = t386_pf_sector, t386_ra_count = t386_pf_count;
                    t386_ra_streak = true;
                    t386_pf_state = PF_IDLE;
                    t386_disk_prefetched++;
                }
            }
        }
        if (t386_ra_of != r || sector < t386_ra_sector || sector + count > t386_ra_sector + t386_ra_count) {
            uint32_t n = sequential ? T386_RA_SECTORS : 8;
            if (n < count) n = count;
            if (n > r->sectors - sector) n = r->sectors - sector;
            t386_ra_of = NULL;
            err = t386_raw_read_into(r, t386_ra, sector, n);
            if (!err) t386_ra_of = r, t386_ra_sector = sector, t386_ra_count = n, t386_ra_streak = sequential;
        } else {
            t386_disk_hits++;
        }
        if (!err) memcpy(buf, t386_ra + (sector - t386_ra_sector) * 512, count * 512);
        t386_ra_next = sector + count;
        // going on sequentially: the next piece read meanwhile, unless it is already
        const uint32_t next = t386_ra_sector + t386_ra_count;
        if (!err && sequential && t386_ra_streak && t386_pf_task && t386_ra_of == r && next < r->sectors &&
            !(t386_pf_state != PF_IDLE && t386_pf_of == r && t386_pf_sector == next)) {
            if (t386_pf_state == PF_BUSY) {   // (another one under way: left to finish)
            } else {
                t386_pf_of = r, t386_pf_sector = next;
                t386_pf_count = r->sectors - next < T386_RA_SECTORS ? r->sectors - next : T386_RA_SECTORS;
                t386_pf_state = PF_BUSY;
                xTaskNotifyGive(t386_pf_task);
            }
        }
        xSemaphoreGive(t386_raw_lock);
        return err ? -1 : 0;
    }
    if (write && t386_ra_of == r && sector < t386_ra_sector + t386_ra_count && sector + count > t386_ra_sector)
        t386_ra_of = NULL;
    if (write) t386_pf_drop(r, sector, count);
    // a big read (DMA) the prefetch has (or is reading): from its buffer
    bool done_already = false;
    const bool sequential = !write && sector == t386_big_next;
    if (!write && t386_pf_state != PF_IDLE && t386_pf_of == r && sector >= t386_pf_sector &&
        sector + count <= t386_pf_sector + t386_pf_count) {
        t386_pf_wait();
        if (t386_pf_state == PF_READY) {
            memcpy(buf, t386_pf + (sector - t386_pf_sector) * 512, count * 512);
            t386_pf_state = PF_IDLE;
            t386_disk_prefetched++;
            done_already = true;
        }
    }
    // straight to and from buf when it is whole cache lines in PSRAM (ide.c's buffer), else through the bounce one
    const bool direct = !((uintptr_t)buf & 127) && esp_ptr_external_ram(buf);
    for (uint32_t done = 0; done < count && !err && !done_already;) {
        uint32_t n = count - done < T386_BOUNCE_SECTORS ? count - done : T386_BOUNCE_SECTORS, run;
        const uint32_t cs = t386_raw_map(r, sector + done, &run);
        if (n > run) n = run;
        uint8_t* p = (uint8_t*)buf + done * 512;
        if (write) {
            if (!direct) memcpy(t386_bounce, p, n * 512);
            esp_cache_msync(direct ? p : t386_bounce, n * 512, ESP_CACHE_MSYNC_FLAG_DIR_C2M);   // (DMA reads PSRAM)
            err = t386_card_rw(r->card, 1, direct ? p : t386_bounce, cs, n);
        } else {
            err = t386_card_rw(r->card, 0, direct ? p : t386_bounce, cs, n);
            if (!err && !direct) memcpy(p, t386_bounce, n * 512);
        }
        done += n;
    }
    if (!write) {
        t386_big_next = sector + count;
        // a streak of big reads: the next one read meanwhile (as many sectors, up to the buffer's)
        const uint32_t next = sector + count;
        if (!err && sequential && t386_pf_task && t386_pf_state != PF_BUSY && next < r->sectors) {
            if (count > t386_buf_sectors) t386_pf_grow();
            t386_pf_of = r, t386_pf_sector = next;
            t386_pf_count = count < t386_buf_sectors ? count : t386_buf_sectors;
            if (t386_pf_count > r->sectors - next) t386_pf_count = r->sectors - next;
            t386_pf_state = PF_BUSY;
            xTaskNotifyGive(t386_pf_task);
        }
    }
    xSemaphoreGive(t386_raw_lock);
    return err ? -1 : 0;
}

// Disk images unbuffered: the guest's writes reach the card at once (not lost if the Tab5 is reset), and the web
// server's file transfers (disk_access.h), which borrow the same FILE while the PC is paused, see no stale buffer.
// An image opened read-only (a CD's, as it is put in) is read as raw card sectors too, once the sketch has said how
// to find its pieces (t386_raw_runs_hook): through FatFs, each seek back in a big image walks its cluster chain from
// the start (~150 FAT sectors in a 600 MB ISO).
static FILE* t386_fopen(const char* path, const char* mode) {
    FILE* f = fopen(path, mode);
    if (f) setvbuf(f, NULL, _IONBF, 0);
    if (f && strcmp(mode, "rb") == 0) t386_raw_attach_file(f, path);
    return f;
}

// ... and closed: its raw access ends with it (a FILE opened later may get the same address)
static int t386_fclose(FILE* f) {
    T386Raw* r = t386_raw_of(f);
    if (r) {
        xSemaphoreTake(t386_raw_lock, portMAX_DELAY);
        if (t386_ra_of == r) t386_ra_of = NULL;
        t386_pf_drop(r, 0, r->sectors);
        heap_caps_free(r->runs);
        memset(r, 0, sizeof(*r));
        xSemaphoreGive(t386_raw_lock);
    }
    return fclose(f);
}

// ide.c's file accesses: raw ones (whole sectors of an attached image) as card sectors, the rest through the
// descriptor (above); a raw image keeps its position in r->pos, which the others start from too.
static int t386_fseeko(FILE* f, off_t off, int whence) {
    T386Raw* r = t386_raw_of(f);
    if (r && whence == SEEK_SET) { r->pos = off; return 0; }
    if (r && whence == SEEK_CUR) { r->pos += off; return 0; }
    const off_t at = lseek(fileno(f), off, whence);
    if (at < 0) return -1;
    if (r) r->pos = at;
    return 0;
}

static off_t t386_ftello(FILE* f) {
    T386Raw* r = t386_raw_of(f);
    return r ? (off_t)r->pos : lseek(fileno(f), 0, SEEK_CUR);
}

static size_t t386_fread(void* buf, size_t size, size_t n, FILE* f) {
    T386Raw* r = t386_raw_of(f);
    const size_t bytes = size * n;
    if (!size) return 0;
    if (r && !(r->pos % 512) && !(bytes % 512)) {
        if (t386_raw_rw(f, 0, buf, r->pos / 512, bytes / 512) != 0) return 0;
        r->pos += bytes;
        return n;
    }
    if (r && lseek(fileno(f), r->pos, SEEK_SET) < 0) return 0;
    const size_t got = t386_fd_io(f, 0, buf, bytes);
    if (r) r->pos += got;
    return got / size;
}

static size_t t386_fwrite(const void* buf, size_t size, size_t n, FILE* f) {
    T386Raw* r = t386_raw_of(f);
    const size_t bytes = size * n;
    if (!size) return 0;
    if (r && !(r->pos % 512) && !(bytes % 512)) {
        if (t386_raw_rw(f, 1, (void*)buf, r->pos / 512, bytes / 512) != 0) return 0;
        r->pos += bytes;
        return n;
    }
    if (r && lseek(fileno(f), r->pos, SEEK_SET) < 0) return 0;
    const size_t put = t386_fd_io(f, 1, (void*)buf, bytes);
    if (r) r->pos += put;
    return put / size;
}

void t386_ram_written(uint32_t addr, int len);   // the bus master's DMA into guest RAM: the translator checks
#define T386_DMA_WRITE(addr, len) t386_ram_written(addr, len)
#define fopen t386_fopen
#define fclose t386_fclose
#define fseeko t386_fseeko
#define ftello t386_ftello
#define fread t386_fread
#define fwrite t386_fwrite
#include "ide.c"
#undef fopen
#undef fclose
#undef fseeko
#undef ftello
#undef fread
#undef fwrite

// Set by the sketch (disk_access.h image_runs): where a card file lies, for t386_fopen's read-only images
int (*t386_raw_runs_hook)(const char* path, T386Run** runs, uint32_t* nruns, uint32_t* sectors);

static int t386_raw_attach_file(FILE* f, const char* path) {
    T386Run* runs;
    uint32_t nruns, sectors;
    if (!t386_raw_runs_hook || !t386_ff_card || !t386_raw_runs_hook(path, &runs, &nruns, &sectors)) return 0;
    return t386_raw_attach(f, t386_ff_card, runs, nruns, sectors);
}

// Tab5 addition: raw access to a CD-ROM drive's disc (a plain image, not CISO) from now on; 1 if it has it (the
// sketch, once the card's pieces can be found: the PC's first disc was opened before)
int t386_ide_cd_attach(IDEIFState* s, int drive, const char* path) {
    IDEState* d = s->drives[drive];
    if (!d || d->drive_kind != IDE_CD || !d->bs || d->bs->read_async != bf_read_async) return 0;
    FILE* f = ((BlockDeviceFile*)d->bs->opaque)->f;
    return f && path && path[0] && t386_raw_attach_file(f, path);
}

// Tab5 addition: the image file behind a hard disk (drive 0/1 of an IDE interface), or NULL (none, or a CD-ROM).
FILE* t386_ide_image(IDEIFState* s, int drive) {
    IDEState* d = s->drives[drive];
    if (!d || d->drive_kind != IDE_HD || !d->bs || d->bs->read_async != bf_read_async)
        return NULL;
    return ((BlockDeviceFile*)d->bs->opaque)->f;
}

// Serial "sdbench": the card's raw read speed (reads only: sdmmc_read_sectors under t386_card_lock, the PC's own
// accesses wait their turn). Each size: MB/s over sequential reads of that many sectors, and the time per command;
// then random 4 KB reads. Into a PSRAM buffer (whole cache lines), as the disk images' reads go, and once into
// internal RAM for comparison.
void t386_card_bench(char* buf, int len) {
    sdmmc_card_t* card = t386_ff_card;
    int o = 0;
    if (!card || !t386_card_lock) { snprintf(buf, len, "sdbench: no card"); return; }
    o += snprintf(buf + o, len - o, "sdbench: %u MB card, %d kHz, %d-bit%s; ", (unsigned)(card->csd.capacity >> 11),
                  card->real_freq_khz, 1 << card->log_bus_width, card->is_ddr ? " DDR" : "");
    const uint32_t big = 2048;   // sectors (1 MB)
    uint8_t* p = heap_caps_aligned_alloc(128, big * 512, MALLOC_CAP_SPIRAM);
    if (!p) { snprintf(buf + o, len - o, "no buffer"); return; }
    const uint32_t cap = card->csd.capacity, base = cap / 4;   // (a quarter in: past the FAT, in the images' area)
    static const uint16_t sizes[] = {1, 8, 32, 128, 256, 2048};
    for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); ++k) {
        const uint32_t n = sizes[k], cmds = n >= 256 ? 8 * 2048 / n : n >= 32 ? 256 : 512;
        esp_err_t e = ESP_OK;
        const int64_t t0 = esp_timer_get_time();
        for (uint32_t i = 0; i < cmds && e == ESP_OK; ++i) {
            xSemaphoreTake(t386_card_lock, portMAX_DELAY);
            e = sdmmc_read_sectors(card, p, base + i * n, n);
            xSemaphoreGive(t386_card_lock);
        }
        const double us = (double)(esp_timer_get_time() - t0);
        o += snprintf(buf + o, len - o, "%u KB x %u: %.2f MB/s, %.0f us each%s; ", (unsigned)(n / 2), (unsigned)cmds,
                      e == ESP_OK ? cmds * n * 512.0 / us : 0.0, us / cmds, e == ESP_OK ? "" : " (error)");
    }
    {   // random 4 KB reads, over the whole card
        uint32_t x = 12345;
        esp_err_t e = ESP_OK;
        const int64_t t0 = esp_timer_get_time();
        for (int i = 0; i < 500 && e == ESP_OK; ++i) {
            x = x * 1664525u + 1013904223u;
            xSemaphoreTake(t386_card_lock, portMAX_DELAY);
            e = sdmmc_read_sectors(card, p, (uint32_t)(((uint64_t)x * (cap - 8)) >> 32) & ~7u, 8);
            xSemaphoreGive(t386_card_lock);
        }
        const double us = (double)(esp_timer_get_time() - t0);
        o += snprintf(buf + o, len - o, "random 4 KB x 500: %.0f us each, %.2f MB/s%s; ", us / 500, 500 * 4096.0 / us,
                      e == ESP_OK ? "" : " (error)");
    }
    heap_caps_free(p);
    uint8_t* q = heap_caps_aligned_alloc(128, 64 * 512, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (q) {   // internal RAM, 32 KB reads
        esp_err_t e = ESP_OK;
        const int64_t t0 = esp_timer_get_time();
        for (uint32_t i = 0; i < 256 && e == ESP_OK; ++i) {
            xSemaphoreTake(t386_card_lock, portMAX_DELAY);
            e = sdmmc_read_sectors(card, q, base + i * 64, 64);
            xSemaphoreGive(t386_card_lock);
        }
        const double us = (double)(esp_timer_get_time() - t0);
        o += snprintf(buf + o, len - o, "internal RAM 32 KB x 256: %.2f MB/s%s", 256 * 32768.0 / us,
                      e == ESP_OK ? "" : " (error)");
        heap_caps_free(q);
    } else {
        o += snprintf(buf + o, len - o, "internal RAM: no 32 KB free");
    }
}
