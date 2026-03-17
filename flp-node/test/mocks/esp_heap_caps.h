#pragma once
/* Mock esp_heap_caps.h — redirects PSRAM alloc to standard heap for host tests */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MALLOC_CAP_SPIRAM  0
#define MALLOC_CAP_DEFAULT 0
#define MALLOC_CAP_8BIT    0

static inline void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}

static inline void *heap_caps_calloc(size_t n, size_t size, uint32_t caps)
{
    (void)caps;
    return calloc(n, size);
}

static inline void heap_caps_free(void *ptr)
{
    free(ptr);
}

static inline size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    (void)caps;
    return 1024 * 1024; /* report 1 MB available */
}
