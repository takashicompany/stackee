#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
extern char g_out[8192]; extern int g_out_len;
// ★ 段階 4 で console の出口が 2 つ (CDC と Raw HID) になった。
//   ホストビルドは dev プロファイル相当にする (CDC がある)。
#ifndef CFG_TUD_CDC
#define CFG_TUD_CDC 1
#endif
static inline bool tud_cdc_connected(void){return true;}
static inline bool tud_mounted(void){return true;}
static inline uint32_t tud_cdc_available(void){return 0;}
static inline uint32_t tud_cdc_read(void*b,uint32_t n){(void)b;(void)n;return 0;}
static inline void tud_cdc_write_char(char c){g_out[g_out_len++]=c;}
static inline uint32_t tud_cdc_write_str(const char*s){while(*s)g_out[g_out_len++]=*s++;return 0;}
static inline uint32_t tud_cdc_write(const void*b,uint32_t n){const char*p=b;for(uint32_t i=0;i<n;i++)g_out[g_out_len++]=p[i];return n;}
static inline uint32_t tud_cdc_write_flush(void){return 0;}
