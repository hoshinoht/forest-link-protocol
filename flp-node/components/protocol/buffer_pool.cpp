#include "buffer_pool.hpp"

#include <cassert>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "buf_pool";

namespace flp
{

void BufferPool::init()
{
    /* Idempotent: free previous allocation if re-initialised */
    if (slabs_)
    {
        heap_caps_free(slabs_);
        slabs_ = nullptr;
    }

    /* Allocate slab array in PSRAM (falls back to internal if unavailable) */
    slabs_ = static_cast<BufferSlab *>(heap_caps_calloc(
        POOL_SIZE, sizeof(BufferSlab),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!slabs_)
    {
        ESP_LOGW(TAG, "PSRAM alloc failed, falling back to internal RAM");
        slabs_ = static_cast<BufferSlab *>(
            heap_caps_calloc(POOL_SIZE, sizeof(BufferSlab), MALLOC_CAP_8BIT));
    }
    assert(slabs_);

    /*
     * Construct each slab (starts C++ object lifetime for the atomics)
     * and build the Treiber free-stack: slab[0] -> slab[1] -> ... -> slab[N-1].
     * top_ points to the last slab (stack grows toward index 0).
     */
    for (uint8_t i = 0; i < POOL_SIZE; i++)
    {
        new (&slabs_[i]) BufferSlab{};
        slabs_[i].next_free.store(
            (i + 1 < POOL_SIZE) ? static_cast<int32_t>(i + 1) : -1,
            std::memory_order_relaxed);
    }
    /* Head of free list is slab[0] */
    top_.store(0, std::memory_order_release);

    ESP_LOGI(TAG,
             "BufferPool initialized: %u slabs x %uB = %uB (budget=%uB) in %s",
             POOL_SIZE,
             static_cast<unsigned>(sizeof(BufferSlab)),
             static_cast<unsigned>(POOL_SIZE * sizeof(BufferSlab)),
             static_cast<unsigned>(POOL_BUDGET_BYTES),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0 ? "PSRAM"
                                                             : "internal");
}

BufferSlab *BufferPool::acquire()
{
    int32_t t = top_.load(std::memory_order_acquire);
    while (t >= 0)
    {
        int32_t next = slabs_[t].next_free.load(std::memory_order_relaxed);
        if (top_.compare_exchange_weak(
                t, next, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            slabs_[t].refcount.store(1, std::memory_order_relaxed);
            slabs_[t].len = 0;
            return &slabs_[t];
        }
        /* CAS failed, t was reloaded by compare_exchange_weak — retry */
    }
    /*
     * Do not call ESP_LOGW here — acquire() is called from the ESP-NOW
     * receive callback (WiFi driver task), and ESP_LOGW takes a logging mutex
     * which can cause priority inversion.  The mesh task logs the counter
     * periodically via get_exhaustion_count().
     */
    pool_exhaustion_count_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void BufferPool::release(BufferSlab *slab)
{
    if (!slab || !slabs_)
    {
        return;
    }

    /* Bounds check: slab must be within our pool */
    ptrdiff_t offset = slab - slabs_;
    if (offset < 0 || offset >= static_cast<ptrdiff_t>(POOL_SIZE))
    {
        pool_overflow_count_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    /*
     * CAS-loop decrement: atomically refuse to decrement past zero.
     * Prevents the TOCTOU race where two concurrent release() calls both
     * see refcount != 0, both fetch_sub, and one wraps uint8_t to 255.
     */
    uint8_t cur = slab->refcount.load(std::memory_order_relaxed);
    while (cur > 0)
    {
        if (slab->refcount.compare_exchange_weak(
                cur, cur - 1,
                std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            if (cur == 1)
            {
                /* Refcount reached zero — push back onto Treiber stack */
                int32_t idx = static_cast<int32_t>(offset);
                int32_t old_top = top_.load(std::memory_order_relaxed);
                do
                {
                    slabs_[idx].next_free.store(
                        old_top, std::memory_order_relaxed);
                } while (!top_.compare_exchange_weak(
                    old_top, idx,
                    std::memory_order_acq_rel, std::memory_order_relaxed));
            }
            return;
        }
        /* CAS failed, cur was reloaded — retry */
    }
    /* refcount was already 0 — do not log (may be WiFi task context) */
    pool_overflow_count_.fetch_add(1, std::memory_order_relaxed);
}

void BufferPool::add_ref(BufferSlab *slab)
{
    if (slab)
    {
        slab->refcount.fetch_add(1, std::memory_order_relaxed);
    }
}

} /* namespace flp */
