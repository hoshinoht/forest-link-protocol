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

} /* namespace flp */
