#include "buffer_pool.hpp"
#include "esp_log.h"

static const char *TAG = "buf_pool";

namespace flp
{

void BufferPool::init()
{
    for (uint8_t i = 0; i < POOL_SIZE; i++)
    {
        slabs_[i].refcount.store(0, std::memory_order_relaxed);
        freelist_[i] = static_cast<int8_t>(i);
    }
    top_.store(POOL_SIZE - 1, std::memory_order_release);
    ESP_LOGI(TAG, "BufferPool initialized: %u slabs (~%u bytes)",
             POOL_SIZE, (unsigned)(POOL_SIZE * sizeof(BufferSlab)));
}

BufferSlab *BufferPool::acquire()
{
    int8_t t = top_.load(std::memory_order_acquire);
    while (t >= 0)
    {
        if (top_.compare_exchange_weak(t, t - 1,
                                       std::memory_order_acq_rel))
        {
            int8_t idx = freelist_[t];
            slabs_[idx].refcount.store(1, std::memory_order_relaxed);
            slabs_[idx].len = 0;
            return &slabs_[idx];
        }
    }
    ESP_LOGW(TAG, "Pool exhausted, no slabs available");
    return nullptr;
}

void BufferPool::release(BufferSlab *slab)
{
    if (!slab)
        return;

    uint8_t prev = slab->refcount.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1)
    {
        // Return to pool
        int8_t idx = static_cast<int8_t>(slab - slabs_);
        int8_t t = top_.load(std::memory_order_acquire);
        int8_t new_top;
        do
        {
            new_top = t + 1;
            freelist_[new_top] = idx;
        } while (!top_.compare_exchange_weak(t, new_top,
                                              std::memory_order_acq_rel));
    }
}

void BufferPool::add_ref(BufferSlab *slab)
{
    if (slab)
    {
        slab->refcount.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace flp
