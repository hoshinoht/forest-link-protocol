#pragma once

#include <cstddef>
#include <cstdint>

namespace flp {

/* / Mount SD card via SPI. Returns true on success. */
bool sdcard_init();

/*
 * / Read entire file into a PSRAM-allocated buffer.
 * / Caller must free *buf_out with free() when done.
 * / Returns true on success.
 */
bool sdcard_read_file(const char *path, uint8_t **buf_out, size_t *size_out);

} /* namespace flp */
