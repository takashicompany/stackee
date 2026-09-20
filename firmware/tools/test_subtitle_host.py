#!/usr/bin/env python3
"""字幕を Mac 上で突き合わせる。**実機には触らない。**

確かめるのは 4 つ:

  1. **本体の実体そのもの** (main/stackee_font16.c / stackee_draw.c /
     stackee_crc32.c) を Mac 用にビルドし、本物の assets/font16.bin を
     通して描いた帯の CRC32 が、tools/subtitle_expected.py の期待値と
     1 ビットも違わないこと (日本語 15 桁・半角混在・字形なし〓・空)。
  2. font16.bin の索引 — tools/gen_font16.py が作った表を、C の二分探索と
     Python の二分探索が同じ字形として引けること。**東雲 BDF から作り直して**
     同じバイト列になること (生成が決定的であること) も見る。
  3. 桁数 (画素数) の計算。全角 16 px / 半角 8 px / 字形なしは 〓 の 16 px。
  4. 帯の幅 (15 桁 = 240 px) を超える字は描かず、途中で切らないこと。

  python3 firmware/tools/test_subtitle_host.py
  python3 -m pytest firmware/tools/test_subtitle_host.py
"""
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

IDF = Path(__file__).resolve().parents[1]
FONT16 = IDF / 'assets/font16.bin'
SHINONOME = IDF / 'assets/src/fonts/shinonome'
sys.path.insert(0, str(IDF / 'tools'))
import gen_font16                                  # noqa: E402
import subtitle_expected as sub                    # noqa: E402

SOURCES = ['stackee_font16.c', 'stackee_draw.c', 'stackee_icons.c',
           'stackee_bdf.c', 'stackee_font8x8.c', 'stackee_crc32.c']
SANITIZE = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']

_BINARY = None


def binary():
    global _BINARY
    if _BINARY is None:
        out = Path(tempfile.mkdtemp()) / 'subtitle'
        cmd = (['cc', '-O1', '-std=gnu11', '-Wall', '-Werror'] + SANITIZE +
               ['-I', str(IDF / 'main'), '-o', str(out),
                str(IDF / 'hostbuild/subtitle_main.c')]
               + [str(IDF / 'main' / s) for s in SOURCES])
        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError('ホストビルドに失敗:\n' + build.stderr)
        _BINARY = out
    return _BINARY


def run(script, font=FONT16):
    out = subprocess.run([str(binary()), str(font)], input=script,
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise AssertionError('実行に失敗:\n' + out.stderr)
    return out.stdout


class BandTest(unittest.TestCase):
    """C が描いた帯と Python の期待値を CRC32 で突き合わせる。"""

    @classmethod
    def setUpClass(cls):
        cls.font = gen_font16.load(FONT16)
        cls.expected = sub.expected(cls.font)

    def test_every_case_matches(self):
        script = ''.join('band %s\n' % text for _n, text in sub.CASES)
        rows = [line.split() for line in run(script).splitlines()]
        self.assertEqual(len(rows), len(sub.CASES))
        for row, case in zip(rows, self.expected['cases']):
            self.assertEqual(row[0], 'band')
            self.assertEqual(int(row[1]), case['crc'],
                             '%s: 帯の CRC が違う' % case['name'])
            self.assertEqual(int(row[2]), case['px'], case['name'])

    def test_the_empty_band_is_the_screen_background(self):
        # 空 = 帯を消す。黒い帯を残さない (白 240x30)。
        white = sub.rgb565_bytes(0xFFFFFF) * sub.WIDTH * sub.SUB_HEIGHT
        self.assertEqual(sub.render(self.font, '').crc(), zlib.crc32(white))
        row = run('band \n').split()
        self.assertEqual(int(row[1]), zlib.crc32(white))

    def test_a_drawn_band_is_not_the_empty_one(self):
        crcs = {c['name']: c['crc'] for c in self.expected['cases']}
        self.assertNotEqual(crcs['日本語15桁'], crcs['空'])
        # 16 桁目は帯からはみ出すので描かない = 15 桁の絵と同じになる。
        self.assertEqual(crcs['日本語15桁'], crcs['はみ出し16桁'])
        # それ以外は全部違う絵 (同じ CRC が並んだら描き分けていない)。
        others = [v for k, v in crcs.items() if k != 'はみ出し16桁']
        self.assertEqual(len(set(others)), len(others))

    def test_geometry_is_the_same_on_both_sides(self):
        rows = dict()
        for line in run('info\n').splitlines():
            parts = line.split()
            rows[parts[0]] = parts[1:]
        y, h, cols, width = (int(v) for v in rows['band_geom'])
        self.assertEqual((y, h, cols, width),
                         (sub.SUB_Y, sub.SUB_HEIGHT, sub.SUB_COLS, sub.SUB_WIDTH))
        self.assertEqual(rows['info'][0], 'ok')
        self.assertEqual(int(rows['info'][1]), gen_font16.HEIGHT)
        self.assertEqual(int(rows['info'][2]), len(self.font.narrow_codes))
        self.assertEqual(int(rows['info'][3]), len(self.font.wide_codes))
        self.assertEqual(int(rows['info'][4]), len(self.font.data))

    def test_the_band_is_exactly_the_bottom_thirty_rows(self):
        self.assertEqual(sub.SUB_Y + sub.SUB_HEIGHT, 320)
        self.assertEqual(sub.SUB_Y, 50 + 240)       # 顔の真下 (重ならない)


class WidthTest(unittest.TestCase):
    """桁数 = 画素数。全角 16 / 半角 8 / 字形なしは 〓 の 16。"""

    @classmethod
    def setUpClass(cls):
        cls.font = gen_font16.load(FONT16)

    def px(self, text):
        return int(run('px %s\n' % text).split()[1])

    def test_full_width_is_sixteen(self):
        self.assertEqual(self.px('あ'), 16)
        self.assertEqual(self.px('あいうえおかきくけこさしすせそ'), 240)

    def test_half_width_is_eight(self):
        self.assertEqual(self.px('A'), 8)
        self.assertEqual(self.px('ABCDEFGHIJ'), 80)
        self.assertEqual(self.px('ｱｲｳ'), 24)        # 半角カタカナ

    def test_fifteen_columns_is_the_screen_width(self):
        self.assertEqual(sub.SUB_COLS * 16, 240)
        # 全角 1 桁 = 半角 2 つ。どちらで数えても 15 桁は 240 px。
        self.assertEqual(self.px('あ' * 15), self.px('A' * 30))

    def test_a_missing_glyph_costs_a_full_width(self):
        self.assertEqual(self.px('☃'), 16)     # ☃ は東雲に無い
        self.assertEqual(self.px('\U0001F600'), 16)  # BMP の外

    def test_python_and_c_agree(self):
        for _name, text in sub.CASES:
            self.assertEqual(self.px(text), sub.text_px(self.font, text), text)

    def test_fit_never_splits_a_character(self):
        text = 'あいうえおかきくけこさしすせそた'      # 16 桁 (256 px)
        self.assertEqual(int(run('fit 240 %s\n' % text).split()[1]),
                         len('あいうえおかきくけこさしすせそ'.encode()))
        # 端数の画素では 1 字も足さない (8 px 足りなければ半角も入らない)。
        self.assertEqual(int(run('fit 7 A\n').split()[1]), 0)
        self.assertEqual(int(run('fit 8 A\n').split()[1]), 1)
        self.assertEqual(int(run('fit 15 あ\n').split()[1]), 0)


class IndexTest(unittest.TestCase):
    """font16.bin の索引 (生成 → 読み → 二分探索)。"""

    @classmethod
    def setUpClass(cls):
        cls.font = gen_font16.load(FONT16)

    def test_header_and_crc(self):
        data = FONT16.read_bytes()
        self.assertEqual(data[:8], b'STKFNT16')
        self.assertEqual(len(data), len(self.font.data))
        # 生成物そのものの CRC (壊れていたら本体は読み込みを断る)。
        self.assertEqual(zlib.crc32(data[gen_font16.HEADER_BYTES:]), self.font.crc)

    def test_codes_are_sorted_and_unique(self):
        for codes in (self.font.narrow_codes, self.font.wide_codes):
            self.assertEqual(codes, sorted(codes))
            self.assertEqual(len(codes), len(set(codes)))
        # 半角と全角で同じ符号を二重に持たない (探す順番で結果が変わる)。
        self.assertEqual(set(self.font.narrow_codes) & set(self.font.wide_codes),
                         set())

    def test_the_expected_characters_are_there(self):
        for ch in 'あアA0亜漢〓、。ｱ':
            self.assertIsNotNone(self.font.glyph(ord(ch)), ch)
        self.assertEqual(self.font.glyph(ord('A'))[0], 8)
        self.assertEqual(self.font.glyph(ord('あ'))[0], 16)
        self.assertIsNone(self.font.glyph(0x2603))

    def test_tofu_replaces_a_missing_glyph(self):
        self.assertEqual(self.font.glyph_or_tofu(0x2603),
                         self.font.glyph(gen_font16.TOFU))

    def test_c_and_python_find_the_same_glyphs(self):
        # 端 (最初と最後) と境目をまたぐ字を、両方の二分探索で引く。
        probes = ([self.font.narrow_codes[0], self.font.narrow_codes[-1],
                   self.font.wide_codes[0], self.font.wide_codes[-1]] +
                  [ord(c) for c in 'あアA0亜漢〓、。ｱ'] + [0x2603, 0x0000])
        script = ''.join('glyph %04X\n' % cp for cp in probes)
        rows = [line.split() for line in run(script).splitlines()]
        self.assertEqual(len(rows), len(probes))
        for cp, row in zip(probes, rows):
            self.assertEqual(int(row[1], 16), cp)
            got = self.font.glyph(cp)
            if got is None:
                self.assertEqual(row[2], '-', hex(cp))
                continue
            width, bits = got
            self.assertEqual(int(row[2]), width, hex(cp))
            if width == 8:
                want = bytes(bits).hex().upper()
            else:
                want = b''.join(v.to_bytes(2, 'big') for v in bits).hex().upper()
            self.assertEqual(row[3], want, hex(cp))

    def test_glyph_count_and_size(self):
        self.assertEqual(self.font.count(),
                         len(self.font.narrow_codes) + len(self.font.wide_codes))
        # 半角 8x16 は 16 B、全角 16x16 は 32 B。索引は符号の 2 B だけ。
        want = (gen_font16.HEADER_BYTES +
                len(self.font.narrow_codes) * (2 + 16) +
                len(self.font.wide_codes) * (2 + 32))
        self.assertEqual(len(self.font.data), want)

    @unittest.skipUnless((SHINONOME / 'shnmk16.bdf').is_file(),
                         '東雲 BDF が見つからない')
    def test_the_generator_is_deterministic(self):
        # 生成物が BDF から作り直したものと 1 バイトも違わないか。
        out = subprocess.run(
            [sys.executable, str(IDF / 'tools/gen_font16.py'), '--check'],
            capture_output=True, text=True)
        self.assertEqual(out.returncode, 0, out.stdout + out.stderr)


class BrokenFontTest(unittest.TestCase):
    """壊れた font16.bin を掴まされても落ちない (帯だけ出す)。"""

    def broken(self, mutate):
        data = bytearray(FONT16.read_bytes())
        mutate(data)
        path = Path(tempfile.mkdtemp()) / 'font16.bin'
        path.write_bytes(bytes(data))
        return path

    def check_refused(self, path):
        rows = dict()
        for line in run('info\nband あ\n', font=path).splitlines():
            parts = line.split()
            rows[parts[0]] = parts[1:]
        self.assertEqual(rows['info'][0], 'bad')
        # 字は出ないが黒帯は出る (画面も会話も止まらない)。
        self.assertEqual(int(rows['band'][1]), 0)      # 画素数 0 = 字形が引けない
        return int(rows['band'][0])                    # 帯の CRC

    def test_a_wrong_magic_is_refused(self):
        self.check_refused(self.broken(lambda d: d.__setitem__(slice(0, 8), b'XXXXXXXX')))

    def test_a_flipped_bit_is_caught_by_the_crc(self):
        def flip(d):
            d[len(d) // 2] ^= 0x01
        self.check_refused(self.broken(flip))

    def test_a_truncated_file_is_refused(self):
        def cut(d):
            del d[len(d) - 64:]
        self.check_refused(self.broken(cut))

    def test_a_black_band_is_still_drawn(self):
        crc = self.check_refused(
            self.broken(lambda d: d.__setitem__(slice(0, 8), b'XXXXXXXX')))
        black = sub.rgb565_bytes(0x000000) * sub.WIDTH * sub.SUB_HEIGHT
        self.assertEqual(crc, zlib.crc32(black))


if __name__ == '__main__':
    unittest.main(verbosity=2)
