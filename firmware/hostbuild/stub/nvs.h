#pragma once
#include "esp_err.h"
#include <stdint.h>
typedef int nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
static inline esp_err_t nvs_open(const char*n,nvs_open_mode_t m,nvs_handle_t*h){(void)n;(void)m;*h=1;return ESP_OK;}
static inline esp_err_t nvs_get_u8(nvs_handle_t h,const char*k,uint8_t*v){(void)h;(void)k;(void)v;return ESP_FAIL;}
static inline void nvs_close(nvs_handle_t h){(void)h;}
