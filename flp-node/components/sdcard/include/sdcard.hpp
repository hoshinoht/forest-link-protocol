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
 * PSRAM read-ahead cache for SD card files.
 *
 * Keeps a 64 KB window in PSRAM; fragment reads within the window are
 * served via memcpy (~0 us) instead of SPI round-trips (~2-5 ms each).
 * The window auto-refills from SD when a request falls outside it.
 *
 * Typical improvement: 240-byte fragment reads hit SD card once per
 * ~273 fragments instead of every fragment.
 */
class SdReadCache
{
  public:
    static constexpr size_t CACHE_SIZE = 64 * 1024; /* 64 KB in PSRAM */

    SdReadCache() = default;
    ~SdReadCache();

    /* Non-copyable, non-movable (owns PSRAM + FILE*) */
    SdReadCache(const SdReadCache &) = delete;
    SdReadCache &operator=(const SdReadCache &) = delete;

    /*
     * Open a file and allocate the PSRAM cache.
     * Returns ESP_OK on success, ESP_ERR_NO_MEM / ESP_ERR_NOT_FOUND on failure.
     */
    esp_err_t open(const char *path);

    /* Total file size (valid after open()) */
    size_t file_size() const { return file_size_; }

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
    bool refill(size_t offset);

    FILE *file_ = nullptr;
    uint8_t *cache_buf_ = nullptr; /* PSRAM-allocated */
    size_t file_size_ = 0;
    size_t cache_start_ = 0;       /* file offset of first cached byte */
    size_t cache_len_ = 0;         /* valid bytes currently in cache */
    uint32_t hits_ = 0;
    uint32_t misses_ = 0;
};

} /* namespace flp */
