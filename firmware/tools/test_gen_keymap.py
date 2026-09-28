#!/usr/bin/env python3
"""キーマップ生成 (tools/gen_keymap.py) のテスト。実機に触らない。

    python3 firmware/tools/test_gen_keymap.py
    python3 -m pytest firmware/tools/test_gen_keymap.py

見ているのは 3 つ:

  1. 生成物が最新か (keymap.py を触って生成し直し忘れていないか)
  2. KMK のキーが QMK のキーコードへ正しく化けているか
  3. VIA 定義 JSON の customKeycodes の並びと、実装の独自キーコードの
     並びが一致しているか (VIA は QK_KB_0 から順に対応づける約束なので、
     ここがずれると VIA 上で別のキーとして表示される)
"""
import json
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
IDF = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import stackee_tree as tree  # noqa: E402
import gen_keymap  # noqa: E402

# 1 と 2 は移植元 (現行 CircuitPython 版の firmware/kmk) を読んで出す。
# 非公開なので、公開リポジトリだけの clone では飛ばす。3 は生成物そのものを
# 読むだけなので、どちらの木でも走る。
NEEDS_KMK = unittest.skipIf(tree.KMK is None,
                            '移植元 (firmware/kmk) が無いので生成し直せない')


@NEEDS_KMK
class GeneratedFilesAreUpToDateTest(unittest.TestCase):
    def test_no_pending_regeneration(self):
        keymap_c, keycodes_h, via, _grid, _conv = gen_keymap.generate()
        pairs = (
            (gen_keymap.OUT_KEYMAP, keymap_c),
            (os.path.join(IDF, 'main', 'qmk_port', 'stackee_keycodes.h'), keycodes_h),
            (gen_keymap.OUT_VIA, json.dumps(via, ensure_ascii=False, indent=2) + '\n'),
        )
        for path, text in pairs:
            with open(path, encoding='utf-8') as handle:
                self.assertEqual(handle.read(), text,
                                 '%s が古い。gen_keymap.py を走らせ直すこと'
                                 % os.path.basename(path))


@NEEDS_KMK
class ConversionTest(unittest.TestCase):
    """KMK のキー -> QMK のキーコード。"""

    @classmethod
    def setUpClass(cls):
        KC, keymap = gen_keymap.load_kmk()
        cls.KC = KC
        cls.keymap = keymap
        cls.conv = gen_keymap.Converter(KC, gen_keymap.load_qmk_basic_names())

    def code(self, key):
        return self.conv.convert(key).code

    def expr(self, key):
        return self.conv.convert(key).expr

    def test_basic_keys_keep_their_hid_usage(self):
        self.assertEqual(self.code(self.KC.Q), 0x14)
        self.assertEqual(self.code(self.KC.ENT), 0x28)
        self.assertEqual(self.code(self.KC.F13), 0x68)

    def test_jis_keys(self):
        # ★ これが本題。adafruit_ble の 0x89 上限を避けるために自前の記述子を
        #   使っている理由そのもの (DESIGN.md §2)。
        self.assertEqual(self.code(self.KC.LANG1), 0x90)
        self.assertEqual(self.code(self.KC.LANG2), 0x91)
        self.assertEqual(self.code(self.KC.INT1), 0x87)
        self.assertEqual(self.code(self.KC.INT3), 0x89)
        self.assertEqual(self.code(self.KC.NUHS), 0x32)

    def test_modified_keys(self):
        self.assertEqual(self.code(self.KC.LSFT(self.KC.SCLN)), 0x0233)
        self.assertEqual(self.expr(self.KC.LSFT(self.KC.SCLN)),
                         'LSFT(KC_SEMICOLON)')
        # KMK の別名 (PLUS = LSFT(EQUAL)) も同じ道を通る。
        self.assertEqual(self.code(self.KC.PLUS), 0x022E)
        self.assertEqual(self.code(self.KC.LGUI(self.KC.INT3)), 0x0889)

    def test_mod_tap(self):
        # KMK の HT(tap, hold) -> QMK の MT(mod, tap)
        self.assertEqual(self.code(self.KC.HT(self.KC.Z, self.KC.LSFT)), 0x221D)
        self.assertEqual(self.expr(self.KC.HT(self.KC.Z, self.KC.LSFT)),
                         'MT(MOD_LSFT, KC_Z)')

    def test_layer_tap_and_momentary(self):
        self.assertEqual(self.code(self.KC.LT(3, self.KC.R)), 0x4315)
        self.assertEqual(self.code(self.KC.MO(5)), 0x5225)

    def test_transparent_and_no(self):
        self.assertEqual(self.code(self.KC.TRNS), 0x0001)
        self.assertEqual(self.code(self.KC.NO), 0x0000)

    def test_holdtap_flags_follow_kmk_defaults(self):
        # KMK の HT は prefer_hold=True が既定 (kmk/modules/holdtap.py)。
        ht = self.conv.convert(self.KC.HT(self.KC.Z, self.KC.LSFT))
        self.assertTrue(ht.hold_on_other_key_press)
        self.assertFalse(ht.permissive_hold)
        # LT は prefer_hold=False が既定 (kmk/modules/layers.py lt_key)。
        lt = self.conv.convert(self.KC.LT(3, self.KC.R))
        self.assertFalse(lt.hold_on_other_key_press)
        # 明示した場合はそちらが勝つ。
        lt_ph = self.conv.convert(
            self.KC.LT(2, self.KC.SPC, prefer_hold=True, tap_interrupted=False))
        self.assertTrue(lt_ph.hold_on_other_key_press)
        self.assertFalse(lt_ph.permissive_hold)

    def test_modified_tap_goes_to_an_extra_keycode(self):
        # QMK の MT() はタップ側を 1 バイトしか持てない。
        out = self.conv.convert(self.KC.HT(self.KC.LSFT(self.KC.SCLN), self.KC.LSFT))
        self.assertEqual(out.kind, 'ext_mt')
        self.assertEqual(out.expr, 'STK_MT_0')
        self.assertEqual(self.conv.ext_mods[0][0], 'MOD_LSFT')
        self.assertEqual(self.conv.ext_mods[0][3], 0x0233)


@NEEDS_KMK
class MatrixTest(unittest.TestCase):
    """生成した keymaps[][][] が配線表どおりか。"""

    @classmethod
    def setUpClass(cls):
        _c, _h, cls.via, cls.grid, cls.conv = gen_keymap.generate()
        _KC, cls.keymap = gen_keymap.load_kmk()

    def test_six_layers_of_five_by_ten(self):
        self.assertEqual(len(self.grid), 6)
        for layer in self.grid:
            self.assertEqual(len(layer), 5)
            for row in layer:
                self.assertEqual(len(row), 10)

    def test_every_wired_slot_is_filled_and_the_rest_are_kc_no(self):
        wired = set(self.keymap.COORD_MAPPING)
        self.assertEqual(len(wired), 43)
        for slot in range(50):
            row, col = divmod(slot, 10)
            cell = self.grid[0][row][col]
            if slot in wired:
                continue
            self.assertEqual(cell.code, 0x0000,
                             'スロット %d は配線が無いのに埋まっている' % slot)

    def test_a_few_positions_match_keymap_py(self):
        # keymap.py の ASSIGN で キー 1 = (0,0)、キー 42 = (4,5)。
        self.assertEqual(self.keymap.ASSIGN[1], (0, 0))
        self.assertEqual(self.keymap.ASSIGN[42], (4, 5))
        self.assertEqual(self.grid[0][0][0].expr, 'LT(4, KC_Q)')
        self.assertEqual(self.grid[0][4][5].expr, 'STK_TALK')

    def test_the_bottom_right_key_is_the_mic_key(self):
        # ★ KMK 側は KC.F13 のまま。この木の KEYMAP_OVERRIDES で差し替える。
        self.assertEqual(self.grid[0][3][9].expr, 'MIC(KC_F13)')
        self.assertEqual(gen_keymap.KEYMAP_OVERRIDES[(0, 3, 9)], 'MIC(KC_F13)')
        # KMK 側 (移植元) はいまも F13。差し替えたのはこの木だけ。
        self.assertEqual(self.grid[1][3][9].expr, 'KC_TRNS')


class ViaDefinitionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(gen_keymap.OUT_VIA, encoding='utf-8') as handle:
            cls.via = json.load(handle)
        with open(os.path.join(IDF, 'main', 'qmk_port', 'stackee_keycodes.h'),
                  encoding='utf-8') as handle:
            cls.header = handle.read()

    def test_new_custom_keys_go_after_the_mod_taps(self):
        """VIA の番号は NVS に保存されている。途中に足すとうしろがずれる。"""
        names = [c['name'] for c in self.via['customKeycodes']]
        self.assertEqual(names.index('STK_MT_0'), 7)        # 0x7E07 のまま
        self.assertEqual(names.index('STK_CUSTOM_0'), 8)    # 0x7E08 (2026-09-29)
        self.assertEqual(names[-1], 'STK_CUSTOM_9')         # 0x7E11
        self.assertEqual(len(names), 18)
        # ヘッダの enum と VIA の並びが 1 つずつ同じか。
        for index, name in enumerate(names):
            self.assertIn('%-20s = QK_KB_0 + %d,' % (name, index), self.header)
        self.assertIn('#define STACKEE_KEYCODE_LAST  (QK_KB_0 + %d)'
                      % (len(names) - 1), self.header)

    def test_every_custom_key_is_visible_in_remap(self):
        """★ Remap は customKeycodes を先頭 32 個 (0x7E00..0x7E1F) しか出さない。

        2026-09-29 まで Custom (旧 CSTM) は 0x7E20.. にいて Remap に出なかった。
        """
        self.assertLessEqual(len(self.via['customKeycodes']),
                             gen_keymap.REMAP_CUSTOM_MAX)
        self.assertEqual(gen_keymap.REMAP_CUSTOM_MAX, 32)

    def test_the_named_mic_entries_are_gone(self):
        """MIC の名前付きの入口 (MIC_F13.. / MIC_F1..) は 2026-09-29 に廃止。

        MIC は Remap の「カスタム」タブで 0x7F00 | kc を手入力する。
        """
        names = [c['name'] for c in self.via['customKeycodes']]
        self.assertFalse([n for n in names if n.startswith('MIC_')])
        self.assertNotIn('MIC_ALIAS', self.header)
        self.assertNotIn('MIC_F13', self.header)

    def test_the_custom_keys_show_as_custom_n(self):
        """Custom_0〜9 は STK_MT_0 のうしろ。VIA のキーキャップは Custom_n。"""
        entries = self.via['customKeycodes']
        custom = [e for e in entries if e['name'].startswith('STK_CUSTOM_')]
        self.assertEqual([e['name'] for e in custom],
                         ['STK_CUSTOM_%d' % n for n in range(10)])
        self.assertEqual(entries[8:18], custom)
        self.assertEqual(entries[-10:], custom)
        for n, entry in enumerate(custom):
            self.assertEqual(entry['shortName'], 'Custom_%d' % n)
            self.assertTrue(entry['title'].startswith('Custom_%d' % n))
        self.assertIn('#define STACKEE_CUSTOM_FIRST  STK_CUSTOM_0', self.header)
        self.assertIn('#define STACKEE_CUSTOM_COUNT  10', self.header)
        self.assertIn('STK_CUSTOM_0         = QK_KB_0 + 8,', self.header)
        self.assertIn('STK_CUSTOM_9         = QK_KB_0 + 17,', self.header)
        self.assertNotIn('CSTM', self.header)

    def test_a_new_mod_tap_would_stop_the_generator(self):
        """STK_MT_* が増えると Custom の番号がずれる。生成の段階で止める。"""
        self.assertEqual(gen_keymap.user_custom_first_index([object()]), 8)
        with self.assertRaises(SystemExit):
            gen_keymap.user_custom_first_index([object(), object()])

    def test_the_mic_range_does_not_touch_the_other_custom_keys(self):
        """MIC(kc) は QK_USER 側。QK_KB の独自キーと重ならない。"""
        self.assertIn('#define STACKEE_MIC_BASE      0x7F00u', self.header)
        self.assertIn('#define MIC(kc)               '
                      '(STACKEE_MIC_BASE | ((kc) & 0xFFu))', self.header)
        # 0x7F00..0x7FFF は QK_USER (0x7E40..0x7FFF) の中、QK_KB の外。
        self.assertGreater(gen_keymap.MIC_BASE, 0x7E3F)
        self.assertLessEqual(gen_keymap.MIC_BASE | 0xFF, 0x7FFF)
        # README §0-5 の例: MIC(F13) = 7F68、MIC(F1) = 7F3A。
        self.assertEqual(gen_keymap.MIC_BASE | 0x68, 0x7F68)
        self.assertEqual(gen_keymap.MIC_BASE | 0x3A, 0x7F3A)

    def test_matrix_is_five_by_ten(self):
        self.assertEqual(self.via['matrix'], {'rows': 5, 'cols': 10})

    def test_ids_match_the_usb_descriptor(self):
        with open(os.path.join(IDF, 'main', 'stackee_usb.c'), encoding='utf-8') as h:
            usb = h.read()
        vid = re.search(r'#define STACKEE_USB_VID\s+(0x[0-9A-Fa-f]+)', usb).group(1)
        pid = re.search(r'#define STACKEE_USB_PID\s+(0x[0-9A-Fa-f]+)', usb).group(1)
        self.assertEqual(int(self.via['vendorId'], 16), int(vid, 16))
        self.assertEqual(int(self.via['productId'], 16), int(pid, 16))

    def test_layout_covers_every_wired_key_exactly_once(self):
        labels = [item for row in self.via['layouts']['keymap'] for item in row
                  if isinstance(item, str)]
        self.assertEqual(len(labels), 43)
        self.assertEqual(len(set(labels)), 43)
        for label in labels:
            row, col = (int(part) for part in label.split(','))
            self.assertTrue(0 <= row < 5 and 0 <= col < 10)

    def test_custom_keycodes_line_up_with_the_implementation(self):
        # ★ VIA は customKeycodes[i] を QK_KB_0 + i に対応づける。
        #   並びがずれると VIA 上で別のキーとして表示される。
        names = [entry['name'] for entry in self.via['customKeycodes']]
        enum = re.findall(r'^\s+(STK_\w+)\s+= QK_KB_0 \+ (\d+),',
                          self.header, flags=re.M)
        self.assertEqual(len(names), len(enum), 'JSON と enum で個数が違う')
        for index, (name, offset) in enumerate(enum):
            self.assertEqual(int(offset), index)
            self.assertEqual(name, names[index],
                             'customKeycodes[%d] が実装と違う' % index)

    def test_every_custom_keycode_has_a_title_and_short_name(self):
        for entry in self.via['customKeycodes']:
            self.assertTrue(entry['title'])
            self.assertTrue(entry['shortName'])


class KeycodesDocTest(unittest.TestCase):
    """tools/keycodes.md が実装と食い違っていないか。"""

    def test_every_custom_key_is_documented(self):
        path = os.path.join(HERE, 'keycodes.md')
        with open(path, encoding='utf-8') as handle:
            doc = handle.read()
        for name, _kmk, _title, _short in gen_keymap.CUSTOM_KEYS:
            self.assertIn(name, doc, '%s が keycodes.md に無い' % name)
        self.assertIn('STK_CUSTOM_0', doc)
        self.assertIn('STK_CUSTOM_9', doc)


if __name__ == '__main__':
    unittest.main(verbosity=2)
