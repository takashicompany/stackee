#!/usr/bin/env python3
"""上段アイコンのシート (assets/status_icons.bin) の末尾に、本体だけのタイルを足す。

**実機には触らない。音も鳴らさない。** 書くのは firmware/assets の 2 ファイルだけ。

  python3 firmware/tools/add_status_icons.py           # 作り直して書く
  python3 firmware/tools/add_status_icons.py --check   # 書かずに一致だけ見る

★ 先頭 18 枚 (電池・音量・Wi-Fi・BLE/USB) は親リポジトリの
  firmware/kmk/tools/generate_status_assets.py が作ったもので、ここでは
  **1 ビットも変えない** (今のシートから切り出してそのまま使う)。
  足すのは assets/src/icons/ の SVG (Material Icons の mic を元にしたもの) だけ。
  ラスタライズは generate_status_assets.py と同じ手順:
  rsvg-convert で 24x24 → アルファを 4 階調 (0 = 透明) → 1 画素 2 bit
  (画素 0 が最上位) → 縦に積む → zlib (9)。

足す順 = main/stackee_icons.h の STACKEE_TILE_MIC_ON, STACKEE_TILE_MIC_X。
"""
import argparse
import hashlib
import io
import json
import subprocess
import sys
import zlib
from pathlib import Path

from PIL import Image

FW = Path(__file__).resolve().parents[1]
ASSETS = FW / 'assets'
SRC = ASSETS / 'src' / 'icons'

SIZE = 24
BPP = 2
TILE_BYTES = SIZE * BPP // 8 * SIZE
BASE_TILES = 18

# (タイル名, SVG, 意味)。★ 並びを変えたら stackee_icons.h も変えること。
EXTRA = [
    ('mic_on', 'mic_on.svg', 'PC 用マイク (UAC) が使える'),
    ('mic_x', 'mic_x.svg', 'PC 用マイクが使えない (USB ホストに繋がっていない / dev プロファイル)'),
]
MATERIAL_MIC = {
    'icon': 'mic', 'path': 'src/av/mic/materialicons/24px.svg',
    'commit': '40a7a292a79d9394157e1ea24f83d52d5e17c556', 'license': 'Apache-2.0',
}


def rasterize(svg):
    """generate_status_assets.py の rasterize と同じ。"""
    text = svg.read_text()
    if 'viewBox="0 0 24 24"' not in text:
        raise ValueError('unexpected viewBox: ' + str(svg))
    png = subprocess.run(['rsvg-convert', '-w', str(SIZE), '-h', str(SIZE), '-f', 'png', str(svg)],
                         check=True, capture_output=True).stdout
    with Image.open(io.BytesIO(png)) as image:
        alpha = image.convert('RGBA').getchannel('A')
    return [(a * 3 + 127) // 255 for a in alpha.tobytes()]


def pack(levels):
    out = bytearray()
    for y in range(SIZE):
        row = levels[y * SIZE:(y + 1) * SIZE]
        for x in range(0, SIZE, 4):
            a, b, c, d = row[x:x + 4]
            out.append((a << 6) | (b << 4) | (c << 2) | d)
    return out


def build():
    sheet = zlib.decompress((ASSETS / 'status_icons.bin').read_bytes())
    if len(sheet) not in (TILE_BYTES * BASE_TILES, TILE_BYTES * (BASE_TILES + len(EXTRA))):
        raise ValueError('status_icons.bin の大きさが想定外: %d B' % len(sheet))
    manifest = json.loads((ASSETS / 'status_icons.json').read_text())
    tiles = manifest['icons']['tiles'][:BASE_TILES]
    if len(tiles) != BASE_TILES or tiles[0]['name'] != 'blank':
        raise ValueError('status_icons.json の先頭 18 枚が想定外')
    out = bytearray(sheet[:TILE_BYTES * BASE_TILES])
    for name, file, meaning in EXTRA:
        path = SRC / file
        out.extend(pack(rasterize(path)))
        tiles.append({'name': name, 'meaning': meaning,
                      'path': 'firmware/assets/src/icons/' + file,
                      'svg_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                      'based_on': MATERIAL_MIC,
                      'tool': 'firmware/tools/add_status_icons.py'})
    manifest['icons']['tiles'] = tiles
    return zlib.compress(bytes(out), 9), json.dumps(manifest, indent=1) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--check', action='store_true', help='書かずに一致だけ見る')
    args = ap.parse_args()
    blob, meta = build()
    targets = [('status_icons.bin', blob), ('status_icons.json', meta.encode())]
    if args.check:
        bad = [n for n, data in targets if (ASSETS / n).read_bytes() != data]
        for n, _ in targets:
            print('%-18s %s' % (n, 'NG (作り直すと変わる)' if n in bad else 'OK'))
        return 1 if bad else 0
    for n, data in targets:
        (ASSETS / n).write_bytes(data)
        print('%-18s %6d bytes' % (n, len(data)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
