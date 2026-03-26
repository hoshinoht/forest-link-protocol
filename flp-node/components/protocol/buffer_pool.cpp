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

    freelist_ = static_cast<int8_t *>(heap_caps_calloc(
        POOL_SIZE, sizeof(int8_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!freelist_)
    {
        freelist_ = static_cast<int8_t *>(
            heap_caps_calloc(POOL_SIZE, sizeof(int8_t), MALLOC_CAP_8BIT));
    }
    assert(freelist_);

    for (uint8_t i = 0; i < POOL_SIZE; i++)
    {
        /* placement-new to initialise atomics in calloc'd memory */
        new (&slabs_[i].refcount) std::atomic<uint8_t>(0);
        slabs_[i].len = 0;
        freelist_[i] = static_cast<int8_t>(i);
    }
    top_.store(POOL_SIZE - 1, std::memory_order_release);
    ESP_LOGI(TAG,
             "BufferPool initialized: %u slabs (~%u bytes) in %s",
             POOL_SIZE,
             (unsigned) (POOL_SIZE * sizeof(BufferSlab)),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0 ? "PSRAM"
                                                             : "internal");
}

BufferSlab *BufferPool::acquire()
{
    int8_t t = top_.load(std::memory_order_acquire);
    while (t >= 0)
    {
        if (top_.compare_exchange_weak(t, t - 1, std::memory_order_acq_rel))
        {
            int8_t idx = freelist_[t];
            slabs_[idx].refcount.store(1, std::memory_order_relaxed);
            slabs_[idx].len = 0;
            return &slabs_[idx];
        }
    }
    /* Fix 12: Do not call ESP_LOGW here — acquire() is called from the ESP-NOW
     * receive callback (WiFi driver task), and ESP_LOGW takes a logging mutex
     * which can cause priority inversion. Increment the atomic counter instead;
     * the mesh task logs it periodically via get_exhaustion_count(). */
    pool_exhaustion_count_.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}

void BufferPool::release(BufferSlab *slab)
{
    if (!slab)
    {
        return;
    }

    /* Load before fetch_sub: if already 0, do not decrement (would wrap to 255
     * on uint8_t, causing the slab to escape the freelist undetected). */
    if (slab->refcount.load(std::memory_order_acquire) == 0)
    {
        ESP_LOGW(TAG, "release called on slab with refcount=0");
        return;
    }

    uint8_t prev = slab->refcount.fetch_sub(1, std::memory_order_acq_rel);

    if (prev == 1)
    {
        /* Return to pool */
        int8_t idx = static_cast<int8_t>(slab - slabs_);
        int8_t t = top_.load(std::memory_order_acquire);
        int8_t new_top;
        do
        {
            new_top = t + 1;
            if (new_top >= POOL_SIZE)
            {
                ESP_LOGW(TAG, "pool overflow while releasing slab");
                return;
            }
            freelist_[new_top] = idx;
        } while (
            !top_.compare_exchange_weak(t, new_top, std::memory_order_acq_rel));
    }
}

void BufferPool::add_ref(BufferSlab *slab)
{
    if (slab)
    {
        slab->refcount.fetch_add(1, std::memory_order_relaxed);
    }
}

} /* namespace flp */
