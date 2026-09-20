#pragma once
// 本物の esp_err.h は stdlib を引っぱってくる。IDF のコードを
// そのまま持ってきて動かすので、代役も同じものが見えるようにする。
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_NOT_FOUND 2
#define ESP_ERR_INVALID_RESPONSE 3
static inline const char *esp_err_to_name(esp_err_t e){(void)e;return "ESP_FAIL";}
