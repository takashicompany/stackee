#pragma once
#include <stdint.h>
static inline uint32_t esp_get_free_heap_size(void){return 212345;}
static inline uint32_t esp_get_minimum_free_heap_size(void){return 198765;}
// 再起動の理由 (段階 4 で status に載せた)。ホストでは POWERON 固定。
typedef enum { ESP_RST_UNKNOWN = 0, ESP_RST_POWERON = 1 } esp_reset_reason_t;
static inline esp_reset_reason_t esp_reset_reason(void){return ESP_RST_POWERON;}
