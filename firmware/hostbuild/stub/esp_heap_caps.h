#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
static inline size_t heap_caps_get_free_size(int c){(void)c;return 8123456;}

// 段階 3: 内蔵 RAM の残りを status に出す (実機では TLS が確保できるかの目安)。
#ifndef MALLOC_CAP_INTERNAL
#define MALLOC_CAP_INTERNAL (1 << 11)
#endif
#ifndef MALLOC_CAP_DMA
#define MALLOC_CAP_DMA      (1 << 3)
#endif
static inline size_t heap_caps_get_minimum_free_size(uint32_t caps) {
    (void)caps;
    return 123456;
}
static inline size_t heap_caps_get_largest_free_block(uint32_t caps) {
    (void)caps;
    return 65536;
}

// 段階 4: fs.put の溜め場を PSRAM に取る。ホストでは普通の malloc。
#ifndef MALLOC_CAP_8BIT
#define MALLOC_CAP_8BIT     (1 << 2)
#endif
static inline void *heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return malloc(size);
}
