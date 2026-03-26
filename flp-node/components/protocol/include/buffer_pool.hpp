#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "packet.hpp"

namespace flp
{

struct BufferSlab
{
    uint8_t data[MAX_MTU];
    size_t len = 0;
    int8_t rssi = 0;
    RxTransport source = RxTransport::ESPNOW;
    std::atomic<uint8_t> refcount{0};
};

class BufferPool
{
  public:
    /*
     * 48 slabs in PSRAM (~12.5 KB).  Doubled from 24 to reduce packet drops
     * during heavy transfers.  PSRAM is fine here — slabs are memcpy'd, not
     * DMA-accessed.
     */
    static constexpr uint8_t POOL_SIZE = 48;

    void init();
    BufferSlab *acquire();
    void release(BufferSlab *slab);
    void add_ref(BufferSlab *slab);

    /*
     * Fix 12: Exposes exhaustion count for periodic logging from the mesh
     * task. Calling ESP_LOGW inside acquire() (which runs from the ESP-NOW
     * receive callback / WiFi task) causes priority inversion via the log
     * mutex. Instead the count is incremented atomically and logged elsewhere.
     */
    uint32_t get_exhaustion_count() const
    {
        return pool_exhaustion_count_.load(std::memory_order_relaxed);
    }

  private:
    std::atomic<uint32_t> pool_exhaustion_count_{0};
    BufferSlab *slabs_ = nullptr;   /* heap_caps_calloc'd in PSRAM */
    int8_t *freelist_ = nullptr;    /* heap_caps_calloc'd in PSRAM */
    std::atomic<int8_t> top_{-1};
};

} /* namespace flp */
