#pragma once
/* Mock esp_timer.h — shadows the real ESP-IDF header for host-native test builds */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Controllable mock time source. Set this in test setUp() before calling any
 * RouteTable methods that internally call esp_timer_get_time(). */
extern int64_t g_mock_time_us;

static inline int64_t esp_timer_get_time(void)
{
    return g_mock_time_us;
}

#ifdef __cplusplus
}
#endif
