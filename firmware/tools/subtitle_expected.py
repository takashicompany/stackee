#!/usr/bin/env python3
"""字幕の帯 (y=290..319) の期待値。**実機に触らない。**

本体と同じ素材 (assets/font16.bin) から、同じ配置・同じ変換で 240x30 の
RGB565 を組み立てて CRC32 を出す。実機の `ui.subtitle` が返す CRC と
tools/check_phase2.py で突き合わせる (顔の CRC 検査と同じ方式)。

  python3 firmware/tools/subtitle_expected.py こんにちは
  python3 firmware/tools/subtitle_expected.py --json

■ 本体と厳密に揃えてあるもの (ずれると照合が無意味になる)
  ・帯の位置    y=290、高さ 30 px、幅は画面と同じ 240 px
  ・1 行の桁数  15 桁 (全角 16 px / 半角 8 px)。はみ出す字は**描かない**
  ・色          文字があれば黒地 (0x000000) に白文字 (0xFFFFFF)、
                空なら画面の地の色 (0xFFFFFF) で塗って帯を消す
  ・縦位置      帯の中央 = 290 + (30 - 16) // 2 = 297
  ・字形が無い字  〓 (U+3013) で代替する
  ・画素の並び  RGB565 / 2 バイト / 上位バイトが先 / 1 行 480 バイト
"""
import argparse
import json
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gen_font16                                  # noqa: E402

IDF = HERE.parent
FONT16 = IDF / 'assets/font16.bin'

WIDTH = 240
STRIDE = WIDTH * 2
SUB_Y = 290
SUB_HEIGHT = 30
SUB_COLS = 15
SUB_WIDTH = SUB_COLS * 16
SUB_BG = 0x000000
SUB_FG = 0xFFFFFF
SCREEN_BG = 0xFFFFFF

# check_phase2.py が実機に投げる文字列。日本語 15 桁・半角混在・字形なし・空。
CASES = [
    ('空', ''),
    ('日本語15桁', 'あいうえおかきくけこさしすせそ'),
    ('はみ出し16桁', 'あいうえおかきくけこさしすせそた'),
    ('半角混在', 'ABC 123 かな漢字'),
    ('記号と句読点', '、。「」！？ー〜'),
    ('字形なし', '絵文字→\U0001F600☃'),
    ('半角カタカナ', 'ｱｲｳｴｵ ﾊﾝｶｸ'),
]


def rgb565_bytes(color):
    r, g, b = (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return bytes(((v >> 8) & 0xFF, v & 0xFF))


class Band:
    """帯だけ (240 x 30) のフレームバッファ。行の並びは実機と同じ。"""

    def __init__(self, color):
        self.buf = bytearray(rgb565_bytes(color) * WIDTH * SUB_HEIGHT)

    def put(self, x, y, color):
        if 0 <= x < WIDTH and 0 <= y < SUB_HEIGHT:
            at = y * STRIDE + x * 2
            self.buf[at:at + 2] = rgb565_bytes(color)

    def crc(self):
        return zlib.crc32(bytes(self.buf))


def render(font, text):
    """main/stackee_draw.c の stackee_draw_subtitle と同じ手順。"""
    if not text:
        return Band(SCREEN_BG)              # 帯を消す = 地の色に戻す
    band = Band(SUB_BG)
    if font is None:
        return band
    top = (SUB_HEIGHT - gen_font16.HEIGHT) // 2
    x = 0
    limit = min(WIDTH, SUB_WIDTH)
    for ch in text:
        got = font.glyph_or_tofu(ord(ch))
        if got is None:
            continue
        width, rows = got
        if x + width > limit:
            break                           # はみ出す字は描かない
        for r, bits in enumerate(rows):
            for col in range(width):
                if bits & (1 << (width - 1 - col)):
                    band.put(x + col, top + r, SUB_FG)
        x += width
    return band


def text_px(font, text):
    return sum(font.advance(ord(ch)) for ch in text)


def expected(font=None):
    font = font or gen_font16.load(FONT16)
    return {
        'y': SUB_Y, 'h': SUB_HEIGHT, 'cols': SUB_COLS, 'width': SUB_WIDTH,
        'stride': STRIDE,
        'font16': {
            'bytes': len(font.data),
            'crc': zlib.crc32(font.data),
            'narrow': len(font.narrow_codes),
            'wide': len(font.wide_codes),
            'glyphs': font.count(),
        },
        'cases': [{'name': name, 'text': text,
                   'crc': render(font, text).crc(),
                   'px': text_px(font, text)}
                  for name, text in CASES],
    }


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('text', nargs='*', help='帯に出す 1 行')
    ap.add_argument('--font', type=Path, default=FONT16)
    ap.add_argument('--json', action='store_true', help='check_phase2 の期待値')
    args = ap.parse_args()

    font = gen_font16.load(args.font)
    if args.json:
        print(json.dumps(expected(font), ensure_ascii=False, indent=1))
        return 0
    text = ' '.join(args.text)
    band = render(font, text)
    print('text  %r' % text)
    print('  px    %d (上限 %d)' % (text_px(font, text), SUB_WIDTH))
    print('  crc32 %d (0x%08X)  y=%d h=%d' % (band.crc(), band.crc(),
                                              SUB_Y, SUB_HEIGHT))
    return 0


if __name__ == '__main__':
    sys.exit(main())
