#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "esp_err.h"

namespace flp {

/* Mount SD card via SPI. Returns ESP_OK on success. */
esp_err_t sdcard_init();

/*
 * Open a file on SD card and return its size.
 * Caller must fclose() the returned handle when done.
 * Returns nullptr on failure.
 */
FILE *sdcard_open(const char *path, size_t *size_out);

/*
 * Read a chunk from an SD card file.
 * Opens the file, seeks to offset, reads len bytes, closes.
 * Returns number of bytes actually read.
 */
size_t sdcard_read_chunk(const char *path,
                         uint8_t *buf,
                         size_t offset,
                         size_t len);

/*
 * PSRAM file cache for SD card files.
 *
 * Allocates min(file_size, MAX_CACHE_SIZE) bytes in PSRAM.  If the
 * entire file fits, it is pre-loaded on open() and every subsequent
 * read is a zero-cost PSRAM memcpy with no SD I/O during transfer.
 * For files larger than MAX_CACHE_SIZE, a sliding window auto-refills
 * from SD on cache miss.
 *
 * With a 512 KB cache and a typical demo file, small files are fully
 * resident in PSRAM after open() — transfer reads never touch SD.
 */
class SdReadCache
{
  public:
    static constexpr size_t MAX_CACHE_SIZE = 512 * 1024;  /* 512 KB cap */
    static constexpr size_t RANDOM_PAGE_SIZE = 4 * 1024;
    static constexpr size_t RANDOM_PAGE_COUNT = 16;

    SdReadCache() = default;
    ~SdReadCache();

    /* Non-copyable, non-movable (owns PSRAM + FILE*) */
    SdReadCache(const SdReadCache &) = delete;
    SdReadCache &operator=(const SdReadCache &) = delete;

    /*
     * Open a file and allocate the PSRAM cache.
     * Allocates min(file_size, MAX_CACHE_SIZE) bytes in PSRAM and
     * pre-loads the file.  Returns ESP_OK on success.
     */
    esp_err_t open(const char *path);

    /* Total file size (valid after open()) */
    size_t file_size() const { return file_size_; }

    /* Actual cache buffer size (may be < MAX_CACHE_SIZE for small files) */
    size_t cache_size() const { return cache_capacity_; }

    /* True if the entire file is resident in PSRAM (no SD I/O on read) */
    bool fully_cached() const { return cache_capacity_ >= file_size_; }

    /*
     * Read `len` bytes starting at file `offset` into `buf`.
     * Returns the number of bytes actually copied.
     * Cache-hit path is a single memcpy from PSRAM.
     */
    size_t read(uint8_t *buf, size_t offset, size_t len);

    /* Cache-hit statistics (optional diagnostics) */
    uint32_t cache_hits() const { return hits_; }
    uint32_t cache_misses() const { return misses_; }

  private:
    struct RandomPageSlot
    {
        size_t page_index = 0;
        size_t valid_len = 0;
        uint32_t stamp = 0;
        bool valid = false;
    };

    bool refill(size_t offset);
    size_t read_via_random_pages(uint8_t *buf, size_t offset, size_t len);
    int find_random_page(size_t page_index) const;
    int choose_random_slot() const;
    bool load_random_page(size_t page_index, int slot);

    FILE *file_ = nullptr;
    uint8_t *cache_buf_ = nullptr;  /* PSRAM-allocated */
    uint8_t *random_cache_buf_ = nullptr;
    size_t file_size_ = 0;
    size_t cache_capacity_ = 0;     /* allocated buffer size */
    size_t cache_start_ = 0;        /* file offset of first cached byte */
    size_t cache_len_ = 0;          /* valid bytes currently in cache */
    RandomPageSlot random_slots_[RANDOM_PAGE_COUNT] = {};
    uint32_t random_clock_ = 0;
    uint32_t random_hits_ = 0;
    uint32_t random_misses_ = 0;
    uint32_t hits_ = 0;
    uint32_t misses_ = 0;
};

} /* namespace flp */
