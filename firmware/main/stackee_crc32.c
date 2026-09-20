#include "stackee_crc32.h"

static uint32_t s_table[256];
static int s_ready;

static void build_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        s_table[i] = c;
    }
    s_ready = 1;
}

uint32_t stackee_crc32(uint32_t crc, const void *data, size_t len) {
    if (!s_ready) {
        build_table();
    }
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        c = s_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}
