#!/usr/bin/env python3
"""字幕の帯 (y=250..319) の期待値。**実機に触らない。**

本体と同じ素材 (assets/font16.bin) から、同じ配置・同じ変換で 240x70 の
RGB565 を組み立てて CRC32 を出す。実機の `ui.subtitle` が返す CRC と
tools/check_phase2.py で突き合わせる (顔の CRC 検査と同じ方式)。

  python3 firmware/tools/subtitle_expected.py こんにちは
  python3 firmware/tools/subtitle_expected.py --json

■ 本体と厳密に揃えてあるもの (ずれると照合が無意味になる)
  ・帯の位置    y=250、高さ 70 px (22 px x 3 行 + 上下に 2 px ずつの余り)、
                幅は画面と同じ 240 px
  ・行の区切り  改行 (\n)。4 行目以降は捨てる (頁めくりは呼び手の仕事)
  ・1 行の桁数  15 桁 (全角 16 px / 半角 8 px)。はみ出す字は**描かない**
  ・色          **いつでも黒地** (0x000000) に白文字 (0xFFFFFF)。字幕が
                無くても白に戻さない (2026-09-21。起動直後から黒)
  ・縦位置      i 行目の字形の上端 = 250 + 2 + i*22 + 3
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
SUB_LINES = 3
SUB_LINE_H = 22
SUB_PAD = (SUB_LINE_H - 16) // 2
SUB_HEIGHT = 70
SUB_Y = 320 - SUB_HEIGHT
SUB_MARGIN = (SUB_HEIGHT - SUB_LINES * SUB_LINE_H) // 2
SUB_COLS = 15
SUB_WIDTH = SUB_COLS * 16
SUB_BG = 0x000000
SUB_FG = 0xFFFFFF
SCREEN_BG = 0xFFFFFF

# 案内の字幕 (main/stackee_talksm.h の STACKEE_TALK_GUIDE_RECORDING /
# _THINKING と**同じ文字列**)。行の割り方はサーバと同じ規則
# (tools/ack_lines.py。tools/test_talk_host.py が突き合わせる)。
GUIDE_RECORDING = 'マイクに向かって\n話しかけてください'
GUIDE_THINKING = '考えています…'

# check_phase2.py が実機に投げる文字列。1 行・3 行・頁めくり直後・空・
# 字形なし・半角混在・案内を含む。
CASES = [
    ('空', ''),
    ('1行', 'あいうえおかきくけこさしすせそ'),
    ('はみ出し16桁', 'あいうえおかきくけこさしすせそた'),
    ('2行', 'いちぎょうめ\nにぎょうめ'),
    ('3行', 'いちぎょうめ\nにぎょうめ\nさんぎょうめ'),
    ('頁めくり直後', 'よんぎょうめ'),
    ('4行目は捨てる', 'あ\nい\nう\nえ'),
    ('半角混在', 'ABC 123 かな漢字'),
    ('記号と句読点', '、。「」！？ー〜'),
    ('字形なし', '絵文字→\U0001F600☃'),
    ('半角カタカナ', 'ｱｲｳｴｵ ﾊﾝｶｸ'),
    ('3行の半角混在', 'ABC 123\nかな漢字\nｱｲｳ ﾊﾝｶｸ'),
    # 帯がいちばん重くなる中身 (3 行 x 15 桁 = 全角 45 字)。描画時間の合否は
    # これで測る (check_phase2.py の SUB_PAINT_MAX_US)。
    ('3行15桁', '\n'.join(['あいうえおかきくけこさしすせそ'] * 3)),
    # 案内の字幕 (main/stackee_talksm.h の STACKEE_TALK_GUIDE_*)。
    # ★ 実機が会話中に出すものそのもの。字形が引けるか (… = U+2026 を含む)
    #   まで含めて、ここで CRC で確かめる。
    ('案内(録音中)', GUIDE_RECORDING),
    ('案内(考え中)', GUIDE_THINKING),
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
    band = Band(SUB_BG)                     # ★ 帯はいつでも黒
    if not text or font is None:
        return band
    limit = min(WIDTH, SUB_WIDTH)
    for i, line in enumerate(text.split('\n')[:SUB_LINES]):
        top = SUB_MARGIN + i * SUB_LINE_H + SUB_PAD
        x = 0
        for ch in line:
            got = font.glyph_or_tofu(ord(ch))
            if got is None:
                continue
            width, rows = got
            if x + width > limit:
                break                       # はみ出す字は描かない
            for r, bits in enumerate(rows):
                for col in range(width):
                    if bits & (1 << (width - 1 - col)):
                        band.put(x + col, top + r, SUB_FG)
            x += width
    return band


def text_px(font, text):
    """1 行ぶんの画素数 (改行を含まない文字列)。"""
    return sum(font.advance(ord(ch)) for ch in text)


def band_px(font, text):
    """帯に要る幅 = いちばん長い行 (main/stackee_draw.c の
    stackee_draw_subtitle_px と同じ)。"""
    if not text:
        return 0
    return max(text_px(font, line) for line in text.split('\n')[:SUB_LINES])


def expected(font=None):
    font = font or gen_font16.load(FONT16)
    return {
        'y': SUB_Y, 'h': SUB_HEIGHT, 'cols': SUB_COLS, 'width': SUB_WIDTH,
        'stride': STRIDE, 'lines': SUB_LINES, 'line_h': SUB_LINE_H,
        'pad': SUB_PAD, 'margin': SUB_MARGIN,
        'font16': {
            'bytes': len(font.data),
            'crc': zlib.crc32(font.data),
            'narrow': len(font.narrow_codes),
            'wide': len(font.wide_codes),
            'glyphs': font.count(),
        },
        'cases': [{'name': name, 'text': text,
                   'crc': render(font, text).crc(),
                   'px': band_px(font, text)}
                  for name, text in CASES],
    }


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('text', nargs='*',
                    help='帯に出す行 (複数なら 1 つ 1 行、最大 3 行)')
    ap.add_argument('--font', type=Path, default=FONT16)
    ap.add_argument('--json', action='store_true', help='check_phase2 の期待値')
    args = ap.parse_args()

    font = gen_font16.load(args.font)
    if args.json:
        print(json.dumps(expected(font), ensure_ascii=False, indent=1))
        return 0
    text = '\n'.join(args.text)
    band = render(font, text)
    print('text  %r' % text)
    print('  px    %d (上限 %d)' % (band_px(font, text), SUB_WIDTH))
    print('  crc32 %d (0x%08X)  y=%d h=%d' % (band.crc(), band.crc(),
                                              SUB_Y, SUB_HEIGHT))
    return 0


if __name__ == '__main__':
    sys.exit(main())
