// CRC-32 (IEEE 802.3、反転、多項式 0xEDB88320)。
//
// ★ Python の zlib.crc32 / binascii.crc32 と**同じ値**になること。
//   段階 2 の合否は「本体が描いた画素の CRC」と「Mac が同じ素材から描いた
//   画素の CRC」の一致で決める (DESIGN.md §6 の「提出の方式」)。両者が
//   同じ多項式・同じバイト順でないと、この検査は意味を失う。
//   ESP-IDF の esp_rom_crc32_le は引数の補数の扱いが版によって紛らわしいので、
//   自前で持つ (表は 1 KB、初回だけ作る)。
#pragma once

#include <stddef.h>
#include <stdint.h>

// crc は 0 から始める。zlib.crc32(data) == stackee_crc32(0, data, len)。
uint32_t stackee_crc32(uint32_t crc, const void *data, size_t len);
