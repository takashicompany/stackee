#!/usr/bin/env python3
"""段階 2 の絵を Mac 上で突き合わせる。**実機には触らない。**

確かめるのは 3 つ:

  1. **本体の描画コードそのもの** (main/stackee_draw.c / stackee_icons.c /
     stackee_bdf.c / stackee_faceanim.c / stackee_crc32.c) を Mac 用に
     ビルドして本物の素材を通し、出てきた CRC32 が
     tools/render_expected.py の期待値と 1 ビットも違わないこと。
     32 表情ぶん + バー 6 状態 + 全面。
  2. その 32 表情は **changes.bin の差分だけで** 作ること
     (顔 0 を全面で描いたあと、1 枚ずつ差分で寄せる)。全面で描いた絵と
     同じ CRC になれば、差分表の読み方が合っている。
  3. タイル番号・色・文字・配置の決め方が firmware/kmk/stackee_icons.py と
     同じこと (電池 20/40/60/80、音量 0/33/66 の境目を含めて総当たり)。

ここが通れば、実機で CRC が合わないときに疑うのは「素材の読み込み
(FAT / zlib 展開)」か「フレームバッファの並び」に絞れる。

  python3 firmware/tools/test_render_host.py
  python3 -m pytest firmware/tools/test_render_host.py
"""
import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

IDF = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(IDF / 'tools'))
import stackee_tree as tree                     # noqa: E402

if tree.KMK is None:
    tree.skip_module('移植元 (firmware/kmk) が無いので突き合わせられない',
                     __name__)
KMK = tree.KMK
import render_expected as expected_mod          # noqa: E402

ASSETS = expected_mod.ASSETS

SOURCES = ['stackee_draw.c', 'stackee_icons.c', 'stackee_bdf.c',
           'stackee_font16.c', 'stackee_faceanim.c', 'stackee_crc32.c',
           'stackee_font8x8.c', 'stackee_selftest.c']

_BINARY = None


def binary():
    global _BINARY
    if _BINARY is None:
        tmp = tempfile.mkdtemp()
        out = Path(tmp) / 'render'
        cmd = (['cc', '-O1', '-std=gnu11', '-Wall', '-Werror',
                '-Wno-unused-function', '-I', str(IDF / 'main'),
                '-o', str(out), str(IDF / 'hostbuild/render_main.c')]
               + [str(IDF / 'main' / s) for s in SOURCES] + ['-lz'])
        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError('ホストビルドに失敗:\n' + build.stderr)
        _BINARY = out
    return _BINARY


def run(*args):
    out = subprocess.run([str(binary()), str(ASSETS)] + list(args),
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise AssertionError('実行に失敗:\n' + out.stderr)
    return out.stdout


def parse_render(text):
    faces, bars, extra = {}, [], {}
    for line in text.splitlines():
        parts = line.split()
        if parts[0] == 'face':
            faces[int(parts[1])] = int(parts[2])
        elif parts[0] == 'bar':
            bars.append((int(parts[1]), int(parts[2]), parts[3]))
        elif parts[0] == 'assets':
            for item in parts[1:]:
                key, value = item.split('=')
                extra[key] = int(value)
        elif parts[0] == 'face_geom':
            extra['face_geom'] = tuple(int(v) for v in parts[1:])
        elif parts[0] in ('all', 'count'):
            extra[parts[0]] = int(parts[1])
    return faces, bars, extra


def load_icons():
    spec = importlib.util.spec_from_file_location(
        'stackee_icons_for_test', KMK / 'stackee_icons.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class RenderTest(unittest.TestCase):
    """C が描いた画素と Python の期待値を CRC32 で突き合わせる。"""

    @classmethod
    def setUpClass(cls):
        cls.faces, cls.bars, cls.extra = parse_render(run())
        cls.expected = expected_mod.Renderer().expected()

    def test_all_32_faces_match(self):
        mismatched = [i for i in range(32)
                      if self.faces[i] != self.expected['faces'][i]]
        self.assertEqual(mismatched, [],
                         '差分で作った顔が全面で描いた顔と違う: %r' % mismatched)

    def test_face_count(self):
        self.assertEqual(self.extra['count'], 32)
        self.assertEqual(self.expected['count'], 32)

    def test_bars_match(self):
        for i, (index, crc, name) in enumerate(self.bars):
            self.assertEqual(index, i)
            self.assertEqual(crc, self.expected['bars'][i], 'バー %s' % name)

    def test_bar_scenarios_are_the_same_table(self):
        # main/stackee_selftest.c と tools/render_expected.py の並びが同じか。
        self.assertEqual([name for _, _, name in self.bars],
                         self.expected['bar_names'])

    def test_whole_framebuffer_matches(self):
        fb = expected_mod.Renderer().framebuffer(
            face=0, bar=len(expected_mod.BAR_SCENARIOS) - 1)
        self.assertEqual(self.extra['all'], fb.crc())

    def test_assets_decode_to_the_same_bytes(self):
        # 実機の ROM tinfl と Mac の zlib が同じものを作るかを、
        # console の ui.assets が返す数字と比べられるようにしてある。
        for key in ('faces_len', 'faces_crc', 'changes_len', 'changes_crc',
                    'icons_len', 'icons_crc'):
            self.assertEqual(self.extra[key], self.expected['assets'][key], key)

    def test_crc32_is_zlib_compatible(self):
        # ★ 本体が持つのは切り詰めたシート (240x200)。素材そのもの
        #   (240x240) は 1 バイトも変わっていないことも一緒に見る。
        import zlib
        raw = zlib.decompress((ASSETS / 'faces.bin').read_bytes())
        self.assertEqual(len(raw), 240 * 240 // 2 * 32)
        self.assertEqual(zlib.crc32(raw), self.expected['assets']['sheet_crc'])
        trimmed, _lost = expected_mod.trim_faces(raw, 32)
        self.assertEqual(len(trimmed), 240 * 200 // 2 * 32)
        self.assertEqual(self.extra['faces_crc'], zlib.crc32(trimmed))

    def test_the_c_and_python_sides_trim_the_same_rows(self):
        # ホストビルドが出す face_geom と期待値側の定数が同じか。
        want = (expected_mod.FACE_Y, expected_mod.FACE_SIZE,
                expected_mod.FACE_TRIM_TOP, expected_mod.FACE_TRIM_BOTTOM,
                expected_mod.FACE_ROWS)
        self.assertEqual(self.extra['face_geom'], want)
        self.assertEqual(self.extra['faces_len'], 240 * 200 // 2 * 32)


class TablesTest(unittest.TestCase):
    """タイル番号・色・文字・配置が stackee_icons.py と同じか (総当たり)。"""

    @classmethod
    def setUpClass(cls):
        cls.icons = load_icons()
        cls.rows = [line.split() for line in run('tables').splitlines()]

    def rows_of(self, kind):
        return [r[1:] for r in self.rows if r[0] == kind]

    def test_battery(self):
        checked = 0
        for pct, chg, tile, color, text in self.rows_of('battery'):
            pct = int(pct)
            value = None if pct < 0 else pct
            charging = chg == '1'
            self.assertEqual(int(tile), self.icons.battery_tile(value, charging),
                             'battery %s chg=%s' % (pct, chg))
            self.assertEqual(int(color, 16),
                             self.icons.battery_color(value, charging))
            self.assertEqual(text, self.icons.battery_text(value))
            checked += 1
        self.assertEqual(checked, 102 * 2)

    def test_volume(self):
        checked = 0
        for pct, tile, color, text in self.rows_of('volume'):
            pct = int(pct)
            self.assertEqual(int(tile), self.icons.volume_tile(pct))
            self.assertEqual(int(color, 16), self.icons.volume_color(pct))
            self.assertEqual(text, self.icons.volume_text(pct))
            checked += 1
        self.assertEqual(checked, 101)

    def test_wifi(self):
        rows = self.rows_of('wifi')
        self.assertTrue(rows)
        for name, tile, color in rows:
            self.assertEqual(int(tile), self.icons.wifi_tile(name), name)
            self.assertEqual(int(color, 16), self.icons.wifi_color(name), name)

    def test_link(self):
        kinds = {'none': None, 'ble': 'ble', 'usb': 'usb'}
        for name, conn, tile, color in self.rows_of('link'):
            kind = kinds[name]
            connected = conn == '1'
            self.assertEqual(int(tile), self.icons.link_tile(kind, connected))
            self.assertEqual(int(color, 16), self.icons.link_color(kind, connected))

    def test_layout(self):
        want = self.icons.layout(240)
        for name, x, y in self.rows_of('layout'):
            self.assertEqual((int(x), int(y)), tuple(want[name]), name)
        self.assertEqual(len(self.rows_of('layout')), len(want))

    def test_shade(self):
        rows = self.rows_of('shade')
        self.assertEqual(len(rows), 16)
        for color, level, out in rows:
            self.assertEqual(int(out, 16),
                             self.icons.shade(int(color, 16), int(level)),
                             'shade(%s, %s)' % (color, level))

    def test_constants(self):
        const = self.rows_of('const')[0]
        values = dict(zip(const[0::2], const[1::2]))
        self.assertEqual(int(values['bar_height']), self.icons.BAR_HEIGHT)
        self.assertEqual(int(values['bar_area']), self.icons.BAR_AREA_HEIGHT)
        self.assertEqual(int(values['char_width']), self.icons.CHAR_WIDTH)
        self.assertEqual(int(values['icon']), self.icons.ICON_SIZE)
        self.assertEqual(int(values['tiles']), len(self.icons.TILES))


class ExpectedUnitTest(unittest.TestCase):
    """期待値を作る側 (render_expected.py) そのものの単体テスト。

    ★ 本体と付き合わせる前に、こちらが素材を正しく読めていることを
      素材の形 (バイト数・BDF の中身) から確かめる。
    """

    @classmethod
    def setUpClass(cls):
        cls.renderer = expected_mod.Renderer()

    def test_asset_sizes(self):
        self.assertEqual(len(self.renderer.faces_raw), 240 * 240 // 2 * 32)
        self.assertEqual(len(self.renderer.faces), 240 * 200 // 2 * 32)
        self.assertEqual(len(self.renderer.changes), 32 * 32 * 4)
        self.assertEqual(len(self.renderer.icons_raw), 6 * 24 * 18)
        self.assertEqual(self.renderer.count, 32)

    def test_bdf(self):
        ascent, glyphs = self.renderer.font
        self.assertEqual(ascent, 22)
        self.assertEqual(sorted(glyphs), sorted(ord(c) for c in '0123456789%-? '))
        w, h, xo, yo, adv, bits = glyphs[ord('0')]
        self.assertEqual((w, h, xo, yo, adv), (12, 24, 0, -2, 12))
        # preview_status_bar.py と同じ並びで読めているか (上 5 行は空白)。
        self.assertEqual([sum(row) for row in bits[:5]], [0, 0, 0, 0, 0])
        self.assertEqual(bits[5], [0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0])

    def test_framebuffer_shape(self):
        fb = self.renderer.framebuffer(face=0, bar=0)
        self.assertEqual(len(fb.buf), 240 * 320 * 2)

    def test_background_is_white_outside_bar_and_face(self):
        fb = self.renderer.framebuffer(face=0, bar=0)
        white = expected_mod.rgb565_bytes(0xFFFFFF)
        # 顔は y=50..249。帯を出していなければ下も白。
        for y in (28, 49, 250, 319):
            row = bytes(fb.buf[y * 480:(y + 1) * 480])
            self.assertEqual(row, white * 240, '行 %d は白のはず' % y)

    def test_face_sits_at_y50_and_ends_at_y249(self):
        # 顔 0 の 1 行目 (= 元絵の 29 行目) が y=50 に入っている (x=0、幅 240)。
        fb = self.renderer.framebuffer(face=0, bar=0)
        table = expected_mod.face_row_table()
        row_bytes = 120
        at = expected_mod.FACE_TRIM_TOP * row_bytes
        first = b''.join(table[b] for b in self.renderer.faces_raw[at:at + row_bytes])
        self.assertEqual(bytes(fb.buf[50 * 480:50 * 480 + 480]), first)
        # 最後の行 (= 元絵の 228 行目) が y=249。
        at = (expected_mod.FACE_SIZE - expected_mod.FACE_TRIM_BOTTOM - 1) * row_bytes
        last = b''.join(table[b] for b in self.renderer.faces_raw[at:at + row_bytes])
        self.assertEqual(bytes(fb.buf[249 * 480:249 * 480 + 480]), last)

    def test_the_trim_keeps_the_sheet_untouched(self):
        # 切り詰めは**描くときだけ**。faces.bin そのものは 240x240 のまま。
        trimmed, lost = expected_mod.trim_faces(self.renderer.faces_raw, 32)
        self.assertEqual(trimmed, self.renderer.faces)
        self.assertEqual(len(lost), 32)

    def test_the_trim_loses_nothing(self):
        """★ 上 29 / 下 11 では **1 画素も落ちない** (32 コマ全数)。

        素材を差し替えたらここが落ちる。そのときは数え直して、
        落ちない切り詰め (= 全コマの余白の最小値) に取り直すこと。
        """
        _trimmed, lost = expected_mod.trim_faces(self.renderer.faces_raw, 32)
        hit = {row['frame']: (row['top'], row['bottom'])
               for row in lost if row['top'] or row['bottom']}
        self.assertEqual(hit, {}, '切り詰めで非背景画素が落ちている: %r' % hit)
        self.assertEqual(self.renderer.expected()['face_lost'], [])

    def test_the_trim_is_as_tight_as_it_can_be(self):
        """29 / 11 は余白の最小値そのもの (1 行でも増やすと欠ける)。"""
        raw = self.renderer.faces_raw
        row_bytes = 120

        def nonbg(frame, y):
            at = frame * 240 * row_bytes + y * row_bytes
            return sum((b >> 4 != 15) + (b & 0x0F != 15)
                       for b in raw[at:at + row_bytes])

        top = min(next(y for y in range(240) if nonbg(f, y)) for f in range(32))
        bottom = min(239 - next(y for y in range(239, -1, -1) if nonbg(f, y))
                     for f in range(32))
        self.assertEqual((top, bottom),
                         (expected_mod.FACE_TRIM_TOP,
                          expected_mod.FACE_TRIM_BOTTOM))

    def test_grey_palette_is_i_times_17(self):
        table = expected_mod.face_row_table()
        # 0x00 = 画素 2 つとも 0 (黒)、0xFF = 2 つとも 15 (白)。
        self.assertEqual(table[0x00], b'\x00\x00' * 2)
        self.assertEqual(table[0xFF], expected_mod.rgb565_bytes(0xFFFFFF) * 2)
        self.assertEqual(table[0xF0][:2], expected_mod.rgb565_bytes(0xFFFFFF))
        self.assertEqual(table[0x0F][:2], b'\x00\x00')

    def test_bar_scenarios_cover_the_interesting_cases(self):
        names = [name for name, _ in expected_mod.BAR_SCENARIOS]
        self.assertEqual(len(names), 6)
        self.assertEqual(len(set(names)), 6)
        states = [state for _, state in expected_mod.BAR_SCENARIOS]
        self.assertTrue(any(s['battery'] is None for s in states), '読めない電池')
        self.assertTrue(any(s['charging'] for s in states), '充電中')
        self.assertTrue(any(s['volume'] == 0 for s in states), 'ミュート')
        self.assertTrue(any(s['battery'] is not None and s['battery'] <= 20
                            for s in states), '20% 以下 (赤)')
        self.assertTrue(any(s['link'] == 'usb' for s in states), 'USB')

    def test_expected_arrays_are_distinct(self):
        exp = self.renderer.expected()
        # 32 表情が全部違う絵になっている (同じ CRC が並んだら素材か
        # 読み方が壊れている)。
        self.assertEqual(len(set(exp['faces'])), 32)
        self.assertEqual(len(set(exp['bars'])), 6)


if __name__ == '__main__':
    unittest.main(verbosity=2)
