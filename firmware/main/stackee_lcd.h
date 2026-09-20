// ILI9342C (CoreS3 の内蔵パネル)。
//
// 転送は firmware/native-lcd と同じ作りにしてある: フレームバッファは PSRAM に
// 置き、描く側は「変わった行」に印を付けて即座に戻る。実際に SPI を叩くのは
// CPU0 の専用タスクで、16 行ずつ DMA で送る。こうしておくと段階 1 以降で
// 入力タスク (CPU1) が描画の SPI 待ちに巻き込まれない。
//
// 向きはパネル側の MADCTL で決める。MV を立てて 240x320 の縦にするので、
// 顔 1 コマの更新で汚れる行が 1/15 になる (native-lcd/LcdFramebuffer.c:36-51)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define STACKEE_LCD_WIDTH   240
#define STACKEE_LCD_HEIGHT  320

// RGB565。フレームバッファにはパネルが受け取る並び (上位バイトが先) で入る。
#define STACKEE_LCD_BLACK   0x0000
#define STACKEE_LCD_WHITE   0xFFFF

esp_err_t stackee_lcd_init(void);
bool stackee_lcd_ready(void);

void stackee_lcd_fill(int x, int y, int w, int h, uint16_t color);

// 内蔵 8x8 フォントで 1 行描く。scale は整数倍 (2 なら 16x16)。
void stackee_lcd_text(int x, int y, const char *text,
                      uint16_t fg, uint16_t bg, int scale);

// 汚れた行をワーカーへ渡す。SPI は待たない。
void stackee_lcd_flush(void);

// ---------------------------------------------------------------------------
// 段階 2: ui タスクがフレームバッファへ直に描く
// ---------------------------------------------------------------------------
// ★ 書いてよいのは ui タスクだけ (DESIGN.md §3 の「フレームバッファは ui が
//   所有」)。描いたら stackee_lcd_mark_rows() で行に印を付け、
//   stackee_lcd_flush() でワーカーへ渡す。
uint8_t *stackee_lcd_framebuffer(void);     // 240*320*2、上位バイトが先
size_t   stackee_lcd_framebuffer_size(void);
void     stackee_lcd_mark_rows(int y, int h);

// 診断用。転送回数・送った行数・ワーカーが動いていた時間 [ms]。
void stackee_lcd_stats(uint32_t *transfers, uint32_t *rows_sent, uint32_t *worker_ms);
