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
    /*
     * Treiber stack link — index of the next free slab, or -1 if tail.
     * Embedded in each slab so that release() writes only to its own
     * (privately-owned) node, eliminating the pre-CAS freelist[] race.
     */
    std::atomic<int32_t> next_free{-1};
};

namespace detail
{
/* Legacy 250-byte slab layout used to preserve roughly the old PSRAM budget. */
struct LegacyBufferSlab
{
    uint8_t data[250];
    size_t len = 0;
    int8_t rssi = 0;
    RxTransport source = RxTransport::ESPNOW;
    std::atomic<uint8_t> refcount{0};
    std::atomic<int32_t> next_free{-1};
};
} /* namespace detail */

class BufferPool
{
  public:
    /*
     * Preserve approximately the pre-1470 PSRAM budget by deriving the slab
     * count from the old 96 x 250-byte layout. Larger MTUs reduce the slab
     * count automatically to keep total allocation bounded.
     */
    static constexpr size_t POOL_BUDGET_BYTES =
        96 * sizeof(detail::LegacyBufferSlab);
    static constexpr uint8_t POOL_SIZE =
        static_cast<uint8_t>((POOL_BUDGET_BYTES / sizeof(BufferSlab)) > 0
                                 ? (POOL_BUDGET_BYTES / sizeof(BufferSlab))
                                 : 1);

    void init();
    BufferSlab *acquire();
    void release(BufferSlab *slab);
    void add_ref(BufferSlab *slab);

    BufferPool() = default;
    BufferPool(const BufferPool &) = delete;
    BufferPool &operator=(const BufferPool &) = delete;

    /*
     * Exposes exhaustion/overflow counts for periodic logging from the mesh
     * task.  Calling ESP_LOGW inside acquire()/release() (which can run from
     * the ESP-NOW receive callback / WiFi task) causes priority inversion via
     * the log mutex.  Counters are incremented atomically and logged elsewhere.
     */
    uint32_t get_exhaustion_count() const
    {
        return pool_exhaustion_count_.load(std::memory_order_relaxed);
    }
    uint32_t get_overflow_count() const
    {
        return pool_overflow_count_.load(std::memory_order_relaxed);
    }

  private:
    std::atomic<uint32_t> pool_exhaustion_count_{0};
    std::atomic<uint32_t> pool_overflow_count_{0};
    BufferSlab *slabs_ = nullptr; /* heap_caps_calloc'd in PSRAM */
    /*
     * Treiber stack head — index into slabs_[], or -1 when empty.
     * int32_t so Xtensa LX7 can use the hardware S32C1I CAS instruction
     * (std::atomic<int8_t> falls back to a libatomic spinlock).
     */
    std::atomic<int32_t> top_{-1};
};

} /* namespace flp */
