#include "buffer_pool.hpp"

#include <cassert>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

    /*
     * Allocate raw slab storage in PSRAM.
     * Avoid heap_caps_calloc() because zeroing the full pool in one long
     * memset can starve IDLE0 during boot and trip task WDT.
     */
    slabs_ = static_cast<BufferSlab *>(heap_caps_malloc(
        POOL_SIZE * sizeof(BufferSlab), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!slabs_)
    {
        ESP_LOGE(TAG, "PSRAM alloc failed for buffer pool (%u x %uB) — cannot continue",
                 POOL_SIZE, static_cast<unsigned>(sizeof(BufferSlab)));
    }
    assert(slabs_);

    /*
     * Construct each slab (starts C++ object lifetime for the atomics)
     * and build the Treiber free-stack: slab[0] -> slab[1] -> ... -> slab[N-1].
     * top_ points to the last slab (stack grows toward index 0).
     */
    for (uint16_t i = 0; i < POOL_SIZE; i++)
    {
        /* Default-initialize: keeps atomics/lifetime correct without zeroing payload bytes. */
        new (&slabs_[i]) BufferSlab;
        slabs_[i].next_free.store(
            (i + 1 < POOL_SIZE) ? static_cast<int32_t>(i + 1) : -1,
            std::memory_order_relaxed);

        /* Yield periodically so IDLE0 can run and feed the task watchdog. */
        if ((i & 0x1F) == 0x1F)
        {
            vTaskDelay(1);
        }
    }
    /* Head of free list is slab[0] */
    top_.store(0, std::memory_order_release);
    free_count_.store(POOL_SIZE, std::memory_order_relaxed);

    ESP_LOGI(TAG,
             "BufferPool initialized: %u slabs x %uB = %uB (budget=%uB) in %s",
             POOL_SIZE,
             static_cast<unsigned>(sizeof(BufferSlab)),
             static_cast<unsigned>(POOL_SIZE * sizeof(BufferSlab)),
             static_cast<unsigned>(POOL_BUDGET_BYTES),
             "PSRAM");
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
            free_count_.fetch_sub(1, std::memory_order_relaxed);
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
                free_count_.fetch_add(1, std::memory_order_relaxed);
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
