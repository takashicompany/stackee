#pragma once
typedef struct { const char *version; } esp_app_desc_t;
static const esp_app_desc_t g_app = {"stackee-idf/0"};
static inline const esp_app_desc_t *esp_app_get_description(void){return &g_app;}
