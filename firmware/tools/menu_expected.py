#!/usr/bin/env python3
"""本体の設定メニュー (y=28..319) の期待値。**実機に触らない。**

本体と同じ素材 (assets/font16.bin) から、同じ配置・同じ変換で 240x292 の
RGB565 を組み立てて CRC32 を出す。見るのは **描き方** (main/stackee_draw.c の
stackee_draw_menu) だけで、中身 (どの行を出すか) は受け取った view のまま使う。

  ・ホスト: tools/test_menu_host.py が hostbuild/menu_main.c の VIEW / CRC と突き合わせる
  ・実機:   tools/check_phase4.py --only menu が `menu.status` の view を
            ここで描き、`lcd.crc y=28 h=292` と突き合わせる

  python3 firmware/tools/menu_expected.py '<menu.status の view の JSON>'

■ 本体と厳密に揃えてあるもの (main/stackee_draw.h の STACKEE_MENU_*)
  ・題     y=28..51 (24 px)、地 0x303030、字 0xFFFFFF、x=6、字の上端 y=32
  ・行     y=52 から 22 px ずつ 11 本。字の上端は行の上 + 3
  ・足もと y=298..319 (22 px)、地 0x303030、字 0xC8C8C8、x=6、字の上端 y=301
  ・行の中 見出しは x=6 から (右端 234 まで。「＞」の行は 214 まで)。
           値は右寄せ、見出しの 8 px 右までに収める。はみ出す字は描かない
  ・色     地 0xFFFFFF / 見出し 0x000000 / 情報の見出し 0x606060 /
           お知らせ 0x0050A0 / 値 0x000000 / 選んでいる行は黒地に白
"""
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
MENU_Y = 28
MENU_H = 320 - MENU_Y
TITLE_H = 24
ROW_H = 22
ROWS_Y = MENU_Y + TITLE_H
FOOT_H = 22
FOOT_Y = 320 - FOOT_H
PAD_X = 6
GAP = 8
VISIBLE = 11
BG = 0xFFFFFF
BAR_BG = 0x303030
TITLE_FG = 0xFFFFFF
FOOT_FG = 0xC8C8C8
TEXT = 0x000000
INFO = 0x606060
NOTE = 0x0050A0
SEL_BG = 0x000000
SEL_FG = 0xFFFFFF
ROW_INFO, ROW_ITEM, ROW_SUB, ROW_NOTE = 0, 1, 2, 3
SUB_MARK = '＞'           # ＞


def rgb565_bytes(color):
    r, g, b = (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF
    v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    return bytes(((v >> 8) & 0xFF, v & 0xFF))


class Region:
    """y=28..319 だけのフレームバッファ。座標は画面のまま渡す。"""

    def __init__(self):
        self.buf = bytearray(rgb565_bytes(BG) * WIDTH * MENU_H)

    def put(self, x, y, color):
        y -= MENU_Y
        if 0 <= x < WIDTH and 0 <= y < MENU_H:
            at = y * STRIDE + x * 2
            self.buf[at:at + 2] = rgb565_bytes(color)

    def fill(self, x, y, w, h, color):
        px = rgb565_bytes(color)
        for row in range(y, y + h):
            r = row - MENU_Y
            if not 0 <= r < MENU_H:
                continue
            at = r * STRIDE + x * 2
            self.buf[at:at + w * 2] = px * w

    def crc(self):
        return zlib.crc32(bytes(self.buf))


def fit(font, text, max_px):
    """max_px に収まる先頭 (stackee_font16_fit と同じ)。(文字列, 画素数)。"""
    px = 0
    out = []
    for ch in text:
        adv = font.advance(ord(ch))
        if px + adv > max_px:
            break
        px += adv
        out.append(ch)
    return ''.join(out), px


def run(region, font, text, x0, top, color, max_px):
    x = 0
    for ch in text:
        got = font.glyph_or_tofu(ord(ch))
        if got is None:
            continue
        width, rows = got
        if x + width > max_px:
            break
        for r, bits in enumerate(rows):
            for col in range(width):
                if bits & (1 << (width - 1 - col)):
                    region.put(x0 + x + col, top + r, color)
        x += width
    return x


def draw_row(region, font, row, selected, y):
    kind, _action, _arg, label, value = row
    top = y + (ROW_H - 16) // 2
    if selected:
        region.fill(0, y, WIDTH, ROW_H, SEL_BG)
    if selected:
        label_rgb = SEL_FG
    elif kind == ROW_INFO:
        label_rgb = INFO
    elif kind == ROW_NOTE:
        label_rgb = NOTE
    else:
        label_rgb = TEXT
    value_rgb = SEL_FG if selected else TEXT
    right = WIDTH - PAD_X
    if kind == ROW_SUB:
        right -= 16
        run(region, font, SUB_MARK, right, top, label_rgb, 16)
        right -= 4
    shown, label_px = fit(font, label, right - PAD_X)
    run(region, font, shown, PAD_X, top, label_rgb, right - PAD_X)
    if not value:
        return
    left = PAD_X + label_px + GAP
    avail = right - left
    if avail <= 0:
        return
    shown, value_px = fit(font, value, avail)
    run(region, font, shown, right - value_px, top, value_rgb, value_px)


def render(font, view):
    """main/stackee_draw.c の stackee_draw_menu と同じ手順。"""
    region = Region()
    region.fill(0, MENU_Y, WIDTH, TITLE_H, BAR_BG)
    region.fill(0, FOOT_Y, WIDTH, FOOT_H, BAR_BG)
    if font is None or view is None:
        return region
    width = WIDTH - 2 * PAD_X
    shown, _ = fit(font, view.get('title', ''), width)
    run(region, font, shown, PAD_X, MENU_Y + (TITLE_H - 16) // 2, TITLE_FG, width)
    shown, _ = fit(font, view.get('footer', ''), width)
    run(region, font, shown, PAD_X, FOOT_Y + (FOOT_H - 16) // 2, FOOT_FG, width)
    rows = view.get('rows') or []
    top = view.get('top', 0)
    selected = view.get('selected', -1)
    for i in range(VISIBLE):
        r = top + i
        if r < 0 or r >= len(rows):
            break
        draw_row(region, font, rows[r], r == selected, ROWS_Y + i * ROW_H)
    return region


def expected_crc(view, font=None):
    font = font or gen_font16.load(FONT16)
    return render(font, view).crc()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    view = json.loads(sys.argv[1])
    print(expected_crc(view))
    return 0


if __name__ == '__main__':
    sys.exit(main())
