// Host regression for sd_mount(), extracted unchanged from the production sketch by test_sd_mount.py.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "sd_transport_config.h"

#if T386_SD_SPI
enum { FSPI = 0 };
static std::vector<std::string> events;
static bool spi_ok, sd_ok, spi_live, sd_live;
static unsigned sd_calls;

struct SPIClass {
    explicit SPIClass(int bus) { assert(bus == FSPI); }
    bool begin(int sck, int miso, int mosi, int cs) {
        assert(sck == 43 && miso == 39 && mosi == 44 && cs == 42);
        events.push_back("spi.begin");
        spi_live = true;  // A failed pin attachment can leave a partially initialized bus.
        return spi_ok;
    }
    void end() {
        assert(!sd_live);
        events.push_back("spi.end");
        spi_live = false;
    }
};

static SPIClass* mounted_spi;
static struct {
    bool begin(int cs, SPIClass& spi, uint32_t hz, const char* mount, int files, bool format) {
        assert(spi_live && spi_ok);
        assert(cs == 42 && hz == 4000000 && files == 5 && !format);
        assert(std::strcmp(mount, "/sdcard") == 0);
        events.push_back("sd.begin");
        ++sd_calls;
        mounted_spi = &spi;
        sd_live = true;
        return sd_ok;
    }
    void end() {
        events.push_back("sd.end");
        sd_live = false;
    }
} SD;
#else
enum esp_reset_reason_t { ESP_RST_POWERON, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT,
                          ESP_RST_TASK_WDT, ESP_RST_BROWNOUT };
enum { SDMMC_FREQ_HIGHSPEED = 40000, SDMMC_FREQ_DEFAULT = 20000 };
static esp_reset_reason_t reset_reason;
static unsigned restarts;
static std::vector<int> frequencies;
static bool high_ok, default_ok;
static esp_reset_reason_t esp_reset_reason() { return reset_reason; }
static void tab5_restart() { ++restarts; }
static struct { void println(const char*) {} } Serial;
static struct {
    bool begin(const char* mount, bool one_bit, bool format, int hz) {
        assert(std::strcmp(mount, "/sdcard") == 0 && !one_bit && !format);
        frequencies.push_back(hz);
        assert(hz == SDMMC_FREQ_HIGHSPEED || hz == SDMMC_FREQ_DEFAULT);
        return hz == SDMMC_FREQ_HIGHSPEED ? high_ok : default_ok;
    }
} SD_MMC;
#endif

#include "production_sd_mount.h"

int main() {
#if T386_SD_SPI
    // A failed SPI begin must not be retried inside SD.begin(), where default pins could be used.
    spi_ok = false;
    sd_ok = true;
    for (int attempt = 0; attempt < 3; ++attempt) {
        events.clear();
        assert(!sd_mount());
        assert(sd_calls == 0 && !spi_live && !sd_live);
        assert(events == (std::vector<std::string>{"spi.begin", "sd.end", "spi.end"}));
    }

    spi_ok = true;
    sd_ok = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        events.clear();
        assert(!sd_mount());
        assert(sd_calls == unsigned(attempt + 1) && !spi_live && !sd_live);
        assert(mounted_spi == &g_sd_spi);
        assert(events == (std::vector<std::string>{"spi.begin", "sd.begin", "sd.end", "spi.end"}));
    }

    // The same SPI object can mount successfully after repeated failures; success keeps it initialized.
    sd_ok = true;
    events.clear();
    assert(sd_mount());
    assert(sd_calls == 4 && spi_live && sd_live && mounted_spi == &g_sd_spi);
    assert(events == (std::vector<std::string>{"spi.begin", "sd.begin"}));
    SD.end();
    g_sd_spi.end();
    std::puts("SPI: initialization failure, mount failure, cleanup order, retry and non-formatting arguments passed");
#else
    const esp_reset_reason_t reasons[] = {ESP_RST_POWERON, ESP_RST_SW, ESP_RST_PANIC,
                                         ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_BROWNOUT};
    for (auto reason : reasons) {
        reset_reason = reason;
        restarts = 0;
        high_ok = true;
        default_ok = true;
        frequencies.clear();
        assert(sd_mount() && restarts == 0);
        assert(frequencies == (std::vector<int>{SDMMC_FREQ_HIGHSPEED}));

        high_ok = false;
        frequencies.clear();
        assert(sd_mount() && restarts == 0);
        assert(frequencies == (std::vector<int>{SDMMC_FREQ_HIGHSPEED, SDMMC_FREQ_DEFAULT}));

        default_ok = false;
        frequencies.clear();
        assert(!sd_mount());
        const bool restart_expected = reason == ESP_RST_SW || reason == ESP_RST_PANIC ||
                                      reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT;
        assert(restarts == unsigned(restart_expected));
        assert(frequencies == (std::vector<int>{SDMMC_FREQ_HIGHSPEED, SDMMC_FREQ_DEFAULT}));
    }
    std::puts("SDMMC: high-speed mount, default-speed fallback, reset policy and non-formatting arguments passed");
#endif
}
