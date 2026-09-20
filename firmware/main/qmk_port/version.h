// QMK の生成物 version.h の代わり。via.c が EEPROM の magic に
// QMK_BUILDDATE を使う (ここが変わると VIA の保存内容が捨てられる)ので、
// **日付ではなく固定文字列** にしてある。ビルドのたびに配列が消えては困る。
#pragma once

#define QMK_VERSION      "stackee-idf/1"
#define QMK_BUILDDATE    "2026-09-16-00:00:00"
#define QMK_KEYBOARD     "stackee"
#define QMK_KEYMAP       "default"
