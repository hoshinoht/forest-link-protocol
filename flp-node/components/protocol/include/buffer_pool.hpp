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
    static constexpr uint8_t POOL_SIZE = 24;

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
    BufferSlab slabs_[POOL_SIZE];
    int8_t freelist_[POOL_SIZE];
    std::atomic<int8_t> top_{-1};
};

} /* namespace flp */
