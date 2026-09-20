#include "stackee_icons.h"

#include <stdio.h>
#include <string.h>

// stackee_icons.py の layout() と 1 行ずつ同じ。
void stackee_icons_layout(int width, stackee_icons_layout_t *out) {
    int right = width - 2;
    int battery_text_x = right - 4 * STACKEE_CHAR_WIDTH;
    int battery_icon_x = battery_text_x - 2 - STACKEE_ICON_SIZE;
    int link_icon_x = battery_icon_x - 2 - STACKEE_ICON_SIZE;
    int wifi_icon_x = link_icon_x - 2 - STACKEE_ICON_SIZE;
    int mid = STACKEE_BAR_HEIGHT / 2;

    out->volume_x = 2;
    out->volume_y = 0;
    out->volume_text_x = 2 + STACKEE_ICON_SIZE + 2;
    out->volume_text_y = mid;
    out->wifi_x = wifi_icon_x;
    out->link_x = link_icon_x;
    out->battery_x = battery_icon_x;
    out->icon_y = 0;
    out->battery_text_x = battery_text_x;
    out->battery_text_y = mid;
}

// ---- 電池 -----------------------------------------------------------------

int stackee_icons_battery_tile(int percent, bool charging) {
    if (percent < 0) {
        return STACKEE_TILE_BATTERY_UNKNOWN;
    }
    if (charging) {
        return STACKEE_TILE_BATTERY_CHARGING;
    }
    if (percent <= 20) { return STACKEE_TILE_BATTERY_1; }
    if (percent <= 40) { return STACKEE_TILE_BATTERY_2; }
    if (percent <= 60) { return STACKEE_TILE_BATTERY_3; }
    if (percent <= 80) { return STACKEE_TILE_BATTERY_4; }
    return STACKEE_TILE_BATTERY_5;
}

uint32_t stackee_icons_battery_color(int percent, bool charging) {
    (void)charging;     // 充電中でも 20% 以下は赤のまま
    if (percent < 0) {
        return STACKEE_ICON_DIM;
    }
    return (percent <= 20) ? STACKEE_ICON_LOW : STACKEE_ICON_FG;
}

void stackee_icons_battery_text(int percent, char *out, int cap) {
    if (percent < 0) {
        snprintf(out, (size_t)cap, "--%%");
    } else {
        snprintf(out, (size_t)cap, "%d%%", percent);
    }
}

// ---- 音量 -----------------------------------------------------------------

int stackee_icons_volume_tile(int percent) {
    if (percent <= 0)  { return STACKEE_TILE_VOLUME_OFF; }
    if (percent <= 33) { return STACKEE_TILE_VOLUME_1; }
    if (percent <= 66) { return STACKEE_TILE_VOLUME_2; }
    return STACKEE_TILE_VOLUME_3;
}

uint32_t stackee_icons_volume_color(int percent) {
    return (percent <= 0) ? STACKEE_ICON_DIM : STACKEE_ICON_FG;
}

void stackee_icons_volume_text(int percent, char *out, int cap) {
    snprintf(out, (size_t)cap, "%d%%", percent);
}

// ---- Wi-Fi ----------------------------------------------------------------

// stackee_icons.py の _WIFI_SEARCHING。「探しに行っている最中」の状態名。
static const char *const WIFI_SEARCHING[] = {
    "load", "radio", "scan_start", "scan_wait", "scan_read", "connect", "linkup",
};

static bool wifi_is_searching(const char *name) {
    if (name == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(WIFI_SEARCHING) / sizeof(WIFI_SEARCHING[0]); i++) {
        if (strcmp(name, WIFI_SEARCHING[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool wifi_is_up(const char *name) {
    return name != NULL && strcmp(name, "up") == 0;
}

int stackee_icons_wifi_tile(const char *state_name) {
    if (wifi_is_up(state_name)) {
        return STACKEE_TILE_WIFI_ON;
    }
    if (wifi_is_searching(state_name)) {
        return STACKEE_TILE_WIFI_SEARCH;
    }
    return STACKEE_TILE_WIFI_OFF;
}

uint32_t stackee_icons_wifi_color(const char *state_name) {
    return wifi_is_up(state_name) ? STACKEE_ICON_FG : STACKEE_ICON_DIM;
}

// ---- BLE / USB ------------------------------------------------------------

int stackee_icons_link_tile(stackee_link_t kind, bool connected) {
    if (kind == STACKEE_LINK_BLE) {
        return connected ? STACKEE_TILE_BLE_CONNECTED : STACKEE_TILE_BLE_ADVERTISING;
    }
    if (kind == STACKEE_LINK_USB) {
        return STACKEE_TILE_USB;
    }
    return STACKEE_TILE_BLANK;
}

uint32_t stackee_icons_link_color(stackee_link_t kind, bool connected) {
    return (kind == STACKEE_LINK_BLE && !connected) ? STACKEE_ICON_DIM : STACKEE_ICON_FG;
}

// ---- 濃さ -----------------------------------------------------------------

uint32_t stackee_icons_shade(uint32_t color, int level, uint32_t bg) {
    int top = (1 << STACKEE_ICON_BPP) - 1;
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        int c = (int)((color >> shift) & 0xFF);
        int b = (int)((bg >> shift) & 0xFF);
        // Python 版と同じ「切り捨て除算」。負の差でも床関数になるように書く。
        int num = (c - b) * level + top / 2;
        int q = num / top;
        if (num % top != 0 && ((num < 0) != (top < 0))) {
            q--;        // C の切り捨ては 0 方向。Python の // は負の無限大方向
        }
        out |= (uint32_t)((b + q) & 0xFF) << shift;
    }
    return out;
}
