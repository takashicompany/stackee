// 上段ステータスバーの「決め方」。firmware/kmk/stackee_icons.py の移植。
//
//   [音量][NN%]          [Wi-Fi][BLE/USB][電池][NN%]
//
// ★ ここには描画が一切入っていない (タイル番号・色・位置・文字を決めるだけ)。
//   CircuitPython 版と同じ切り分けで、Mac のホストビルドでそのまま動かせる。
//   実際に画素を置くのは stackee_draw.c。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define STACKEE_ICON_SIZE        24
#define STACKEE_ICON_BPP         2      // 0 = 透明、1..3 = 塗りの濃さ
#define STACKEE_BAR_HEIGHT       24
#define STACKEE_BAR_AREA_HEIGHT  28     // 黒帯。アイコンの下に 4px の余白
#define STACKEE_CHAR_WIDTH       12
#define STACKEE_ICON_TILES       18

// 1 タイルのバイト数 (24px * 2bit = 6 バイト/行 * 24 行)。
#define STACKEE_ICON_TILE_BYTES  (STACKEE_ICON_SIZE * STACKEE_ICON_BPP / 8 * STACKEE_ICON_SIZE)
#define STACKEE_ICON_SHEET_BYTES (STACKEE_ICON_TILE_BYTES * STACKEE_ICON_TILES)

// ★ 並び順 = シート上のタイル番号 (status_icons.json の tiles と同じ)。
enum {
    STACKEE_TILE_BLANK = 0,
    STACKEE_TILE_BATTERY_1, STACKEE_TILE_BATTERY_2, STACKEE_TILE_BATTERY_3,
    STACKEE_TILE_BATTERY_4, STACKEE_TILE_BATTERY_5,
    STACKEE_TILE_BATTERY_CHARGING, STACKEE_TILE_BATTERY_UNKNOWN,
    STACKEE_TILE_VOLUME_OFF, STACKEE_TILE_VOLUME_1, STACKEE_TILE_VOLUME_2,
    STACKEE_TILE_VOLUME_3,
    STACKEE_TILE_WIFI_ON, STACKEE_TILE_WIFI_SEARCH, STACKEE_TILE_WIFI_OFF,
    STACKEE_TILE_BLE_CONNECTED, STACKEE_TILE_BLE_ADVERTISING, STACKEE_TILE_USB,
};

// 上段バーの色 (24bit RGB)。黒地に白抜き。
#define STACKEE_ICON_BG   0x000000u
#define STACKEE_ICON_FG   0xFFFFFFu
#define STACKEE_ICON_DIM  0x8A939Eu
#define STACKEE_ICON_LOW  0xFF4444u

// 画面全体の背景 (顔の素材が白地前提)。
#define STACKEE_SCREEN_BG 0xFFFFFFu

typedef enum {
    STACKEE_LINK_NONE = 0,      // 不明 (blank)
    STACKEE_LINK_BLE,
    STACKEE_LINK_USB,
} stackee_link_t;

typedef struct {
    int volume_x, volume_y;
    int volume_text_x, volume_text_y;
    int wifi_x, link_x, battery_x, icon_y;
    int battery_text_x, battery_text_y;
} stackee_icons_layout_t;

void stackee_icons_layout(int width, stackee_icons_layout_t *out);

// percent は 0..100、読めないときは -1 (Python 版の None)。
int      stackee_icons_battery_tile(int percent, bool charging);
uint32_t stackee_icons_battery_color(int percent, bool charging);
void     stackee_icons_battery_text(int percent, char *out, int cap);

int      stackee_icons_volume_tile(int percent);
uint32_t stackee_icons_volume_color(int percent);
void     stackee_icons_volume_text(int percent, char *out, int cap);

// stackee_wifi の状態名をそのまま渡す ("up" / "scan_wait" / "off" ...)。
int      stackee_icons_wifi_tile(const char *state_name);
uint32_t stackee_icons_wifi_color(const char *state_name);

int      stackee_icons_link_tile(stackee_link_t kind, bool connected);
uint32_t stackee_icons_link_color(stackee_link_t kind, bool connected);

// 塗りの濃さ level (0..3) を背景とまぜた色。3 で color そのもの。
uint32_t stackee_icons_shade(uint32_t color, int level, uint32_t bg);
