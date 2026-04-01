#include "heap_monitor.hpp"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "heap";

namespace flp
{

void HeapMonitor::init()
{
    log_snapshot("init");
}

void HeapMonitor::log_snapshot(const char *tag)
{
    ESP_LOGI(TAG,
             "[%s] free_internal=%zu free_psram=%zu "
             "min_internal=%zu min_psram=%zu largest_psram=%zu",
             tag,
             free_internal(),
             free_psram(),
             min_free_internal(),
             min_free_psram(),
             largest_free_psram_block());
}

void HeapMonitor::periodic_check()
{
    /* Log every call (~10s cadence set by caller) */
    ESP_LOGI(TAG,
             "free_int=%zu free_ps=%zu min_int=%zu min_ps=%zu",
             free_internal(),
             free_psram(),
             min_free_internal(),
             min_free_psram());
}

size_t HeapMonitor::free_internal() const
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

size_t HeapMonitor::free_psram() const
{
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

size_t HeapMonitor::min_free_internal() const
{
    return heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
}

size_t HeapMonitor::min_free_psram() const
{
    return heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
}

size_t HeapMonitor::largest_free_psram_block() const
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
}

size_t HeapMonitor::serialize(uint8_t *buf, size_t max_len) const
{
    if (max_len < 20)
    {
        return 0;
    }

    uint32_t vals[5] = {
        static_cast<uint32_t>(free_internal()),
        static_cast<uint32_t>(free_psram()),
        static_cast<uint32_t>(min_free_internal()),
        static_cast<uint32_t>(min_free_psram()),
        static_cast<uint32_t>(largest_free_psram_block()),
    };
    memcpy(buf, vals, 20);
    return 20;
}

} /* namespace flp */
