#pragma once

#include <cstddef>
#include <cstdint>

namespace flp
{

class HeapMonitor
{
  public:
    void init();
    void log_snapshot(const char *tag);
    void periodic_check(); /* call every ~10s from mesh_manager::run() */

    size_t free_internal() const;
    size_t free_psram() const;
    size_t min_free_internal() const;
    size_t min_free_psram() const;
    size_t largest_free_psram_block() const;

    /*
     * Serialize heap stats into 20-byte binary for MQTT publish.
     * Format: [free_internal:4][free_psram:4][min_internal:4][min_psram:4][largest_psram_block:4]
     */
    size_t serialize(uint8_t *buf, size_t max_len) const;

};

} /* namespace flp */
