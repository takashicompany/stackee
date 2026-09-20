#pragma once
#include "freertos/FreeRTOS.h"
typedef void* SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){return (void*)1;}
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s,int t){(void)s;(void)t;return pdTRUE;}
static inline void xSemaphoreGive(SemaphoreHandle_t s){(void)s;}
