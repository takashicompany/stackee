#!/usr/bin/env python3
"""段階 2 の期待値。**実機に触らない。**

本体と同じ素材 (firmware/kmk/stackee_assets/) から、同じ配置・同じ変換で
240x320 の RGB565 フレームバッファを組み立て、CRC32 を出す。

  python3 firmware/tools/render_expected.py --json
  python3 firmware/tools/render_expected.py --face 7 --bar 5 --fb /tmp/fb.bin

本体 (`ui.selftest` / `lcd.crc`) との照合は tools/check_phase2.py。
ここが「正しい絵」の定義なので、本体と食い違ったらまずこちらを疑う。

■ 本体と厳密に揃えてあるもの (ずれると照合が無意味になる)
  ・画素の並び  RGB565 / 2 バイト / **上位バイトが先** / 1 行 480 バイト
  ・行の並び    上から下。MADCTL 0xA8 のぶんはパネル側が吸収するので、
                フレームバッファは素直な 240x320 (段階 0 で実機確認済み)
  ・CRC32       zlib.crc32 と同じ (多項式 0xEDB88320、初期値・最終 XOR とも
                0xFFFFFFFF)。本体は main/stackee_crc32.c
  ・帯          y=250..319 は**いつでも黒**。字幕が無くても白に戻さない
  ・顔          4bpp、画素 0 が上位ニブル、パレットは i*17 の等間隔グレー、
                位置は x=0 / y=50。**上 29 行・下 11 行を捨てた 240x200** を置く
                (元絵も faces.bin も変えない。空いた 40 px は字幕 3 行へ。
                 29/11 は 32 コマ全部の余白の最小値 = 1 画素も落ちない)
  ・アイコン    2bpp、画素 0 が最上位 2 ビット、濃さ 0 は透明、
                色は stackee_icons.shade() で黒地とまぜる
  ・文字        status_h24.bdf。baseline = y_mid - box_h//2 + FONT_ASCENT
                (firmware/kmk/tools/preview_status_bar.py の draw_text と同じ)
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import sys
import zlib

IDF = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(IDF / 'tools'))
import stackee_tree as _tree                     # noqa: E402

# 配置と色の唯一の出所は移植元 (現行 CircuitPython 版の stackee_icons.py)。
# 非公開側にしか無いので、無ければ None にして呼び手に判断させる。
KMK = _tree.KMK
# 素材は firmware/assets を先に見る (公開側だけで完結する)。
ASSETS = _tree.ASSETS if (_tree.ASSETS / 'manifest.json').is_file() \
    else _tree.KMK_ASSETS

WIDTH = 240
HEIGHT = 320
STRIDE = WIDTH * 2
FACE_SIZE = 240
# main/stackee_draw.h の STACKEE_FACE_TRIM_TOP / _BOTTOM / STACKEE_FACE_ROWS。
FACE_TRIM_TOP = 29
FACE_TRIM_BOTTOM = 11
FACE_ROWS = FACE_SIZE - FACE_TRIM_TOP - FACE_TRIM_BOTTOM      # 200
FACE_X = 0
FACE_Y = 50                      # stackee_face.py: (height-size)//2 + 10
SCREEN_BG = 0xFFFFFF
# 字幕の帯 (main/stackee_draw.h)。★ **いつでも黒**。字幕が無くても白に
# 戻さない (2026-09-21)。起動直後の 1 枚目から黒い。
SUB_Y = 250
SUB_HEIGHT = 70
SUB_BG = 0x000000

# main/stackee_selftest.c と同じ並び (0..4 は preview_status_bar.py の SCENARIOS)。
BAR_SCENARIOS = [
    ('bat100/vol100/wifi-up/ble-connected',
     dict(battery=100, charging=False, volume=100, wifi='up', link='ble', connected=True)),
    ('bat55-charging/vol50/wifi-scan/ble-adv',
     dict(battery=55, charging=True, volume=50, wifi='scan_wait', link='ble', connected=False)),
    ('bat15/vol0/wifi-off/usb',
     dict(battery=15, charging=False, volume=0, wifi='wait', link='usb', connected=False)),
    ('bat72/vol25/wifi-up/usb',
     dict(battery=72, charging=False, volume=25, wifi='up', link='usb', connected=False)),
    ('bat-unknown/vol5/wifi-boot/link-unknown',
     dict(battery=None, charging=False, volume=5, wifi='boot', link=None, connected=False)),
    ('boot-default',
     dict(battery=None, charging=False, volume=20, wifi='off', link='ble', connected=False)),
]


def load_icons():
    """firmware/kmk/stackee_icons.py をそのまま使う (配置・色の唯一の出所)。"""
    spec = importlib.util.spec_from_file_location('stackee_icons', KMK / 'stackee_icons.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def rgb565(color):
    r = (color >> 16) & 0xFF
    g = (color >> 8) & 0xFF
    b = color & 0xFF
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def rgb565_bytes(color):
    v = rgb565(color)
    return bytes(((v >> 8) & 0xFF, v & 0xFF))


def parse_bdf(path, wanted='0123456789%-? '):
    """preview_status_bar.py の parse_bdf と同じ読み方。"""
    text = path.read_text(encoding='ascii')
    ascent = int(re.search(r'^FONT_ASCENT (-?\d+)$', text, re.M).group(1))
    glyphs = {}
    for block in re.findall(r'STARTCHAR.*?ENDCHAR', text, re.S):
        code = int(re.search(r'^ENCODING (\d+)$', block, re.M).group(1))
        if chr(code) not in wanted:
            continue
        w, h, xo, yo = map(int, re.search(
            r'^BBX (-?\d+) (-?\d+) (-?\d+) (-?\d+)$', block, re.M).groups())
        adv = int(re.search(r'^DWIDTH (-?\d+)', block, re.M).group(1))
        rows = block.split('BITMAP', 1)[1].split()[:h]
        nbits = len(rows[0]) * 4 if rows else 0
        bits = [[(int(r, 16) >> (nbits - 1 - x)) & 1 for x in range(w)] for r in rows]
        glyphs[code] = (w, h, xo, yo, adv, bits)
    missing = [c for c in wanted if ord(c) not in glyphs]
    if missing:
        raise ValueError('BDF に無い文字: %r' % missing)
    return ascent, glyphs


class Framebuffer:
    def __init__(self):
        self.buf = bytearray(STRIDE * HEIGHT)

    def fill(self, x, y, w, h, color):
        row = rgb565_bytes(color) * w
        for yy in range(y, y + h):
            at = yy * STRIDE + x * 2
            self.buf[at:at + w * 2] = row

    def put(self, x, y, color):
        if 0 <= x < WIDTH and 0 <= y < HEIGHT:
            at = y * STRIDE + x * 2
            self.buf[at:at + 2] = rgb565_bytes(color)

    def crc(self, y=0, h=HEIGHT):
        return zlib.crc32(bytes(self.buf[y * STRIDE:(y + h) * STRIDE]))


def face_row_table():
    """4bpp の 1 バイト (画素 2 つ) -> RGB565 4 バイト。"""
    grey = [rgb565_bytes((i * 17) * 0x010101) for i in range(16)]
    return [grey[b >> 4] + grey[b & 0x0F] for b in range(256)]


def trim_faces(raw, count):
    """本体 (main/stackee_ui.c の trim_faces) と同じ切り詰め。

    各コマの上 FACE_TRIM_TOP 行・下 FACE_TRIM_BOTTOM 行を落として
    240x200 に詰め直す。落ちた非背景画素 (地の色 = 濃さ 15) の数も一緒に返す。
    ★ 29 / 11 は 32 コマ全部の余白の最小値なので、ここは全部 0 になる。
    """
    row_bytes = FACE_SIZE // 2
    out = bytearray()
    lost = []
    for f in range(count):
        base = f * FACE_SIZE * row_bytes
        top = bottom = 0
        for y in (list(range(FACE_TRIM_TOP)) +
                  list(range(FACE_SIZE - FACE_TRIM_BOTTOM, FACE_SIZE))):
            row = raw[base + y * row_bytes: base + (y + 1) * row_bytes]
            n = sum((b >> 4 != 15) + (b & 0x0F != 15) for b in row)
            if y < FACE_TRIM_TOP:
                top += n
            else:
                bottom += n
        lost.append({'frame': f, 'top': top, 'bottom': bottom})
        out += raw[base + FACE_TRIM_TOP * row_bytes:
                   base + (FACE_SIZE - FACE_TRIM_BOTTOM) * row_bytes]
    return bytes(out), lost


def draw_face(fb, faces, frame, table):
    """切り詰めたシート (240x200) を y=FACE_Y に置く。"""
    row_bytes = FACE_SIZE // 2
    base = frame * FACE_ROWS * row_bytes
    for y in range(FACE_ROWS):
        src = faces[base + y * row_bytes: base + (y + 1) * row_bytes]
        line = b''.join(table[b] for b in src)
        at = (FACE_Y + y) * STRIDE + FACE_X * 2
        fb.buf[at:at + len(line)] = line


def draw_icon(fb, icons, tiles, index, color, x0, y0):
    size = icons.ICON_SIZE
    for i, level in enumerate(tiles[index]):
        if level:
            fb.put(x0 + i % size, y0 + i // size, icons.shade(color, level))


def decode_icon_sheet(icons, raw):
    """preview_status_bar.py の decode_sheet と同じ (画素 0 が最上位 2 ビット)。"""
    size = icons.ICON_SIZE
    per_row = size // 4
    tiles = []
    for t in range(len(icons.TILES)):
        levels = []
        for y in range(size):
            base = (t * size + y) * per_row
            for byte in raw[base:base + per_row]:
                levels.extend(((byte >> 6) & 3, (byte >> 4) & 3, (byte >> 2) & 3, byte & 3))
        tiles.append(levels)
    return tiles


def draw_text(fb, font, text, color, x, y_mid, box_h):
    ascent, glyphs = font
    baseline = y_mid - box_h // 2 + ascent
    for ch in text:
        w, h, xo, yo, adv, bits = glyphs[ord(ch)]
        top = baseline - (h + yo)
        for r, row in enumerate(bits):
            for c, bit in enumerate(row):
                if bit:
                    fb.put(x + xo + c, top + r, color)
        x += adv


def draw_bar(fb, icons, tiles, font, state):
    pos = icons.layout(WIDTH)
    pct = state['battery']
    charging = state['charging']
    vol = state['volume']
    kind = state['link']
    connected = state['connected']
    fb.fill(0, 0, WIDTH, icons.BAR_AREA_HEIGHT, icons.BG)
    slots = {
        'volume': (icons.volume_tile(vol), icons.volume_color(vol)),
        'wifi': (icons.wifi_tile(state['wifi']), icons.wifi_color(state['wifi'])),
        'link': (icons.link_tile(kind, connected), icons.link_color(kind, connected)),
        'battery': (icons.battery_tile(pct, charging), icons.battery_color(pct, charging)),
    }
    for slot in icons.SLOTS:
        index, color = slots[slot]
        draw_icon(fb, icons, tiles, index, color, *pos[slot])
    draw_text(fb, font, icons.volume_text(vol), icons.volume_color(vol),
              *pos['volume_text'], icons.BAR_HEIGHT)
    draw_text(fb, font, icons.battery_text(pct), icons.battery_color(pct, charging),
              *pos['battery_text'], icons.BAR_HEIGHT)


class Renderer:
    def __init__(self, assets=ASSETS):
        self.icons = load_icons()
        self.faces_raw = zlib.decompress((assets / 'faces.bin').read_bytes())
        self.changes = zlib.decompress((assets / 'changes.bin').read_bytes())
        self.icons_raw = zlib.decompress((assets / 'status_icons.bin').read_bytes())
        self.tiles = decode_icon_sheet(self.icons, self.icons_raw)
        self.font = parse_bdf(assets / 'status_h24.bdf')
        self.table = face_row_table()
        self.count = len(self.faces_raw) // (FACE_SIZE * FACE_SIZE // 2)
        # 本体と同じ切り詰め。以後、絵を組み立てるのはこちらだけを使う。
        self.faces, self.face_lost = trim_faces(self.faces_raw, self.count)

    def framebuffer(self, face=0, bar=len(BAR_SCENARIOS) - 1):
        fb = Framebuffer()
        fb.fill(0, 0, WIDTH, HEIGHT, SCREEN_BG)
        draw_bar(fb, self.icons, self.tiles, self.font, BAR_SCENARIOS[bar][1])
        draw_face(fb, self.faces, face, self.table)
        # 帯は黒 (字幕なし)。全面の CRC に入る。
        fb.fill(0, SUB_Y, WIDTH, SUB_HEIGHT, SUB_BG)
        return fb

    def expected(self):
        """ui.selftest が返すべき配列。"""
        bar_index = len(BAR_SCENARIOS) - 1
        faces = []
        for frame in range(self.count):
            fb = self.framebuffer(face=frame, bar=bar_index)
            faces.append(fb.crc(FACE_Y, FACE_ROWS))
        bars = []
        base = self.framebuffer(face=0, bar=bar_index)
        for i in range(len(BAR_SCENARIOS)):
            fb = Framebuffer()
            fb.buf[:] = base.buf
            draw_bar(fb, self.icons, self.tiles, self.font, BAR_SCENARIOS[i][1])
            bars.append(fb.crc(0, self.icons.BAR_AREA_HEIGHT))
        return {
            'width': WIDTH, 'height': HEIGHT, 'stride': STRIDE,
            'face_x': FACE_X, 'face_y': FACE_Y, 'face_size': FACE_SIZE,
            'face_trim_top': FACE_TRIM_TOP,
            'face_trim_bottom': FACE_TRIM_BOTTOM, 'face_rows': FACE_ROWS,
            'sub_y': SUB_Y, 'sub_height': SUB_HEIGHT, 'sub_bg': SUB_BG,
            'face_lost': [row for row in self.face_lost
                          if row['top'] or row['bottom']],
            'bar_height': self.icons.BAR_AREA_HEIGHT,
            'count': self.count,
            'faces': faces,
            'bars': bars,
            'bar_names': [name for name, _ in BAR_SCENARIOS],
            'assets': {
                # ★ 本体が持つのは切り詰めたシート。ui.assets が返すのも
                #   こちら (素材そのものは 240x240 のまま = sheet_*)。
                'faces_len': len(self.faces),
                'faces_crc': zlib.crc32(self.faces),
                'sheet_len': len(self.faces_raw),
                'sheet_crc': zlib.crc32(self.faces_raw),
                'changes_len': len(self.changes),
                'changes_crc': zlib.crc32(self.changes),
                'icons_len': len(self.icons_raw),
                'icons_crc': zlib.crc32(self.icons_raw),
            },
        }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--face', type=int, default=0)
    ap.add_argument('--bar', type=int, default=len(BAR_SCENARIOS) - 1)
    ap.add_argument('--fb', type=Path, help='フレームバッファを生のまま書き出す')
    ap.add_argument('--json', action='store_true', help='ui.selftest の期待値を出す')
    args = ap.parse_args()

    renderer = Renderer()
    if args.json:
        print(json.dumps(renderer.expected(), indent=1))
        return 0
    fb = renderer.framebuffer(args.face, args.bar)
    if args.fb:
        args.fb.write_bytes(bytes(fb.buf))
        print(args.fb)
    print('face=%d bar=%s' % (args.face, BAR_SCENARIOS[args.bar][0]))
    print('  all  crc32 = 0x%08X' % fb.crc())
    print('  face crc32 = 0x%08X  (y=%d h=%d)' % (fb.crc(FACE_Y, FACE_ROWS), FACE_Y, FACE_ROWS))
    print('  bar  crc32 = 0x%08X  (y=0 h=%d)'
          % (fb.crc(0, renderer.icons.BAR_AREA_HEIGHT), renderer.icons.BAR_AREA_HEIGHT))
    return 0


if __name__ == '__main__':
    sys.exit(main())
