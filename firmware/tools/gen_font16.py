#!/usr/bin/env python3
"""東雲 (Shinonome) の BDF から 16 px の日本語ビットマップフォント
`assets/font16.bin` を作る。**実機に触らない。**

返答音声の字幕 (画面の下の帯、y=290..319) に使う。字幕は 1 行 15 桁
(全角 1 桁 = 16 px、半角 0.5 桁 = 8 px) なので、必要なのは 16 px の
全角と 8x16 の半角の 2 つだけ。

  python3 firmware/tools/gen_font16.py \
      --wide   research/stackee/fonts/shinonome/shnmk16.bdf \
      --narrow research/stackee/fonts/shinonome/shnm8x16r.bdf \
      --out    firmware/assets/font16.bin

★ BDF 本体はリポジトリに入れない (未追跡の research/ にある)。入れるのは
  この道具と生成物だけ。東雲フォントは Public Domain
  (research/stackee/fonts/shinonome/LICENSE.utf8.txt)。

■ 出来上がる形 (すべてリトルエンディアン)

    0  "STKFNT16"          8 B  目印
    8  u16 version = 1
   10  u16 height  = 16         1 字の高さ [px]
   12  u16 narrow_count         8x16 の字数
   14  u16 wide_count           16x16 の字数
   16  u32 narrow_codes_off     u16 の Unicode 表 (昇順) の位置
   20  u32 narrow_bits_off      1 字 16 B (1 行 1 B)
   24  u32 wide_codes_off       u16 の Unicode 表 (昇順) の位置
   28  u32 wide_bits_off        1 字 32 B (1 行 2 B、上位バイトが左)
   32  u32 total_bytes          ファイル全体のバイト数
   36  u32 crc32                40 バイト目から末尾までの CRC32 (zlib と同じ)
   40  ...                      4 つの区画

★ 幅ごとに区画を分けてあるのは、**1 字あたりの位置を掛け算だけで出す**ため。
  字形の位置表を持たずに済むので、表は Unicode (2 B) だけになり、
  索引は 14 KB で足りる (1 字 6 B の表なら 43 KB)。探すのは二分探索
  (O(log n))。本体側の読み方は main/stackee_font16.c。

★ 1 行の並びは BDF の 16 進をそのまま: **左端が最上位ビット**。

■ 文字の選び方
  半角  JIS X 0201 の 0x20..0x7E (ASCII と同じ) と 0xA1..0xDF
        (半角カタカナ U+FF61..U+FF9F)
  全角  JIS X 0208 の全部。JIS → Unicode は Python の codec (euc_jp) で
        変換する (表を手で書かない)。
"""
import argparse
import hashlib
import re
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b'STKFNT16'
VERSION = 1
HEIGHT = 16
HEADER_BYTES = 40

# 字形が無いときに代わりに出す字 (〓 GETA MARK)。本体も同じ値を使う。
TOFU = 0x3013


# ---------------------------------------------------------------------------
# BDF
# ---------------------------------------------------------------------------
def parse_bdf(path):
    """{ENCODING: (w, h, [行の整数])} を返す。BBX は全字同じ前提で検査する。"""
    text = Path(path).read_text(encoding='latin-1')
    out = {}
    for block in re.findall(r'STARTCHAR.*?ENDCHAR', text, re.S):
        m = re.search(r'^ENCODING (-?\d+)$', block, re.M)
        if m is None or int(m.group(1)) < 0:
            continue
        code = int(m.group(1))
        w, h, _xo, _yo = map(int, re.search(
            r'^BBX (-?\d+) (-?\d+) (-?\d+) (-?\d+)$', block, re.M).groups())
        rows = block.split('BITMAP', 1)[1].split()
        rows = [r for r in rows if r != 'ENDCHAR'][:h]
        if len(rows) != h:
            raise ValueError('%s: ENCODING %d の行数が %d しかない' % (path, code, len(rows)))
        out[code] = (w, h, [int(r, 16) for r in rows])
    if not out:
        raise ValueError('%s: 字が 1 つも無い' % path)
    return out


# ---------------------------------------------------------------------------
# JIS -> Unicode
# ---------------------------------------------------------------------------
def jis0208_to_unicode(encoding):
    """BDF の ENCODING (0x2121 形式の区点) を Unicode にする。

    表を手で書かず Python の codec に任せる。JIS X 0208 の 2 バイトに
    0x80 を足すと EUC-JP になるので、それを decode する。
    """
    hi, lo = (encoding >> 8) & 0xFF, encoding & 0xFF
    if not (0x21 <= hi <= 0x7E and 0x21 <= lo <= 0x7E):
        return None
    try:
        ch = bytes((hi | 0x80, lo | 0x80)).decode('euc_jp')
    except UnicodeDecodeError:
        return None
    if len(ch) != 1 or ord(ch) > 0xFFFF:
        return None
    return ord(ch)


def jis0201_to_unicode(encoding):
    """JIS X 0201 の 1 バイト。ASCII の範囲と半角カタカナだけ採る。"""
    if 0x20 <= encoding <= 0x7E:
        return encoding                     # ASCII と同じ位置
    if 0xA1 <= encoding <= 0xDF:
        return 0xFF61 + (encoding - 0xA1)   # 半角カタカナ
    return None


# ---------------------------------------------------------------------------
def collect(bdf, width, to_unicode, source):
    """{Unicode: 行の整数} と、捨てた数を返す。"""
    glyphs = {}
    skipped = 0
    dupes = 0
    for code, (w, h, rows) in sorted(bdf.items()):
        if w != width or h != HEIGHT:
            raise ValueError('%s: ENCODING %d の BBX が %dx%d (期待 %dx%d)'
                             % (source, code, w, h, width, HEIGHT))
        cp = to_unicode(code)
        if cp is None:
            skipped += 1
            continue
        if cp in glyphs:
            dupes += 1
            continue
        glyphs[cp] = rows
    return glyphs, skipped, dupes


def pack(narrow, wide):
    narrow_codes = sorted(narrow)
    wide_codes = sorted(wide)
    n, m = len(narrow_codes), len(wide_codes)
    if n > 0xFFFF or m > 0xFFFF:
        raise ValueError('字が多すぎる')

    narrow_codes_off = HEADER_BYTES
    narrow_bits_off = narrow_codes_off + n * 2
    wide_codes_off = narrow_bits_off + n * HEIGHT
    wide_bits_off = wide_codes_off + m * 2
    total = wide_bits_off + m * HEIGHT * 2

    body = bytearray()
    body += b''.join(struct.pack('<H', c) for c in narrow_codes)
    for c in narrow_codes:
        body += bytes(narrow[c])                    # 1 行 1 バイト
    body += b''.join(struct.pack('<H', c) for c in wide_codes)
    for c in wide_codes:
        for row in wide[c]:
            body += struct.pack('>H', row)          # 1 行 2 バイト (左が上位)
    assert len(body) == total - HEADER_BYTES, (len(body), total - HEADER_BYTES)

    head = bytearray(MAGIC)
    head += struct.pack('<HHHH', VERSION, HEIGHT, n, m)
    head += struct.pack('<IIIII', narrow_codes_off, narrow_bits_off,
                        wide_codes_off, wide_bits_off, total)
    head += struct.pack('<I', zlib.crc32(bytes(body)))
    assert len(head) == HEADER_BYTES, len(head)
    return bytes(head) + bytes(body)


# ---------------------------------------------------------------------------
# 読む側 (道具とテストが使う。本体の main/stackee_font16.c と同じ読み方)
# ---------------------------------------------------------------------------
class Font16:
    def __init__(self, data):
        if len(data) < HEADER_BYTES or data[:8] != MAGIC:
            raise ValueError('font16.bin ではない')
        (version, height, n, m) = struct.unpack_from('<HHHH', data, 8)
        (nc, nb, wc, wb, total) = struct.unpack_from('<IIIII', data, 16)
        (crc,) = struct.unpack_from('<I', data, 36)
        if version != VERSION or height != HEIGHT:
            raise ValueError('版が違う: %d / 高さ %d' % (version, height))
        if total != len(data):
            raise ValueError('長さが違う: %d != %d' % (total, len(data)))
        if zlib.crc32(data[HEADER_BYTES:]) != crc:
            raise ValueError('CRC が合わない')
        self.data = data
        self.height = height
        self.narrow_codes = [struct.unpack_from('<H', data, nc + i * 2)[0]
                             for i in range(n)]
        self.wide_codes = [struct.unpack_from('<H', data, wc + i * 2)[0]
                           for i in range(m)]
        self._nb, self._wb = nb, wb
        self.crc = crc

    def glyph(self, cp):
        """(幅, [行の整数]) か None。行は左端が最上位ビット。"""
        i = _bisect(self.narrow_codes, cp)
        if i is not None:
            at = self._nb + i * HEIGHT
            return 8, list(self.data[at:at + HEIGHT])
        i = _bisect(self.wide_codes, cp)
        if i is not None:
            at = self._wb + i * HEIGHT * 2
            return 16, [struct.unpack_from('>H', self.data, at + r * 2)[0]
                        for r in range(HEIGHT)]
        return None

    def glyph_or_tofu(self, cp):
        return self.glyph(cp) or self.glyph(TOFU)

    def advance(self, cp):
        """その字が進む画素数。字形が無ければ代替 (〓) の幅。"""
        got = self.glyph_or_tofu(cp)
        return got[0] if got else 0

    def text_px(self, text):
        return sum(self.advance(ord(ch)) for ch in text)

    def count(self):
        return len(self.narrow_codes) + len(self.wide_codes)


def _bisect(codes, cp):
    low, high = 0, len(codes)
    while low < high:
        mid = (low + high) // 2
        if codes[mid] < cp:
            low = mid + 1
        elif codes[mid] > cp:
            high = mid
        else:
            return mid
    return None


def load(path):
    return Font16(Path(path).read_bytes())


# ---------------------------------------------------------------------------
def main():
    here = Path(__file__).resolve().parent
    idf = here.parent
    root = idf.parents[1]
    shinonome = root / 'research/stackee/fonts/shinonome'
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--wide', type=Path, default=shinonome / 'shnmk16.bdf',
                    help='JIS X 0208 の 16x16 (既定: research/ の東雲)')
    ap.add_argument('--narrow', type=Path, default=shinonome / 'shnm8x16r.bdf',
                    help='JIS X 0201 の 8x16')
    ap.add_argument('--out', type=Path, default=idf / 'assets/font16.bin')
    ap.add_argument('--check', action='store_true',
                    help='書かずに、いまある生成物と同じになるかだけ見る')
    args = ap.parse_args()

    for path in (args.wide, args.narrow):
        if not path.is_file():
            print('BDF が無い: %s' % path, file=sys.stderr)
            print('東雲 BDF はリポジトリに入れていない。'
                  '置き場を --wide / --narrow で指定する。', file=sys.stderr)
            return 2

    narrow_bdf = parse_bdf(args.narrow)
    wide_bdf = parse_bdf(args.wide)
    narrow, n_skip, n_dupe = collect(narrow_bdf, 8, jis0201_to_unicode, args.narrow)
    wide, w_skip, w_dupe = collect(wide_bdf, 16, jis0208_to_unicode, args.wide)
    if TOFU not in wide:
        raise ValueError('代替の 〓 (U+3013) が全角に無い')

    blob = pack(narrow, wide)
    digest = hashlib.sha256(blob).hexdigest()

    print('半角 %d 字 (捨てた %d / 重複 %d)' % (len(narrow), n_skip, n_dupe))
    print('全角 %d 字 (捨てた %d / 重複 %d)' % (len(wide), w_skip, w_dupe))
    print('合計 %d 字 / %d バイト (%.1f KB)'
          % (len(narrow) + len(wide), len(blob), len(blob) / 1024.0))
    print('sha256 %s' % digest)

    if args.check:
        if not args.out.is_file():
            print('生成物が無い: %s' % args.out, file=sys.stderr)
            return 1
        same = args.out.read_bytes() == blob
        print('生成物は %s' % ('最新' if same else '古い (作り直すこと)'))
        return 0 if same else 1

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(blob)
    print('書いた: %s' % args.out)
    return 0


if __name__ == '__main__':
    sys.exit(main())
