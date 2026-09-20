#!/usr/bin/env python3
"""我々の HID レポート記述子を、**IDF の esp_hid のパーサそのもの**に通す。

    python3 firmware/tools/test_hid_report_map.py

2026-09-16 に実機で `esp_hidd_dev_init()` が ESP_FAIL を返し、BLE が
立ち上がらなかった。原因の最有力は「esp_hid が記述子を読めない」こと。
`esp_hid_parse_report_map()` は純粋な C なので、実機に触らずに同じ判定が
できる。ここで通らないものは実機でも通らない。

記述子のバイト列は **main/stackee_usb.c の実体から抜き出す**
(tools/hid_desc_check.py の extract_arrays)。「USB と BLE で同じ配列」と
いう約束を、テスト側でも崩さないため。
"""
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
IDF = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import hid_desc_check  # noqa: E402

ESP_HID = os.path.join(IDF, 'hostbuild', 'esp_hid')
STUB = os.path.join(IDF, 'hostbuild', 'stub')

# esp_hid_common.h の esp_hid_report_type_t / esp_hid_usage_t より。
REPORT_TYPE = {1: 'INPUT', 2: 'OUTPUT', 3: 'FEATURE'}
USAGE = {0: 'GENERIC', 1: 'KEYBOARD', 2: 'MOUSE', 4: 'JOYSTICK',
         8: 'GAMEPAD', 16: 'CCONTROL'}

_CACHE = {}


def parse_with_esp_hid(name):
    """main/stackee_usb.c の配列 name を esp_hid のパーサに通す。"""
    if name in _CACHE:
        return _CACHE[name]
    arrays = hid_desc_check.extract_arrays(
        os.path.join(IDF, 'main', 'stackee_usb.c'))
    body = arrays[name]

    tmp = tempfile.mkdtemp(prefix='stackee-hidmap-')
    source = os.path.join(tmp, 'map.c')
    with open(source, 'w', encoding='utf-8') as handle:
        handle.write('#include <stddef.h>\n#include <stdint.h>\n')
        handle.write('static const uint8_t MAP[] = {%s};\n'
                     % ','.join(str(b) for b in body))
        handle.write('const uint8_t *stackee_test_report_map(size_t *len) {\n'
                     '    *len = sizeof(MAP);\n    return MAP;\n}\n')
    binary = os.path.join(tmp, 'hidmap')
    cmd = ['cc', '-O1', '-std=gnu11',
           '-I', ESP_HID, '-I', STUB,
           os.path.join(IDF, 'hostbuild', 'hid_report_map_main.c'),
           os.path.join(ESP_HID, 'esp_hid_common.c'),
           os.path.join(STUB, 'esp_log_stub.c'),
           source, '-o', binary]
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode != 0:
        raise AssertionError('ホストビルドに失敗:\n' + done.stderr[-3000:])
    run = subprocess.run([binary], capture_output=True, text=True)
    out = [line for line in run.stdout.splitlines() if line]
    _CACHE[name] = (out, run.stderr)
    return _CACHE[name]


class KeysReportMapTest(unittest.TestCase):
    """キーボード / マウス / コンシューマの記述子 (BLE に渡すもの)。"""

    @classmethod
    def setUpClass(cls):
        cls.out, cls.err = parse_with_esp_hid('s_hid_keys_report')

    def test_esp_hid_can_parse_it(self):
        # ★ ここが通らないと esp_hidd_dev_init() が ESP_FAIL を返し、
        #   BLE が丸ごと立ち上がらない (2026-09-16 の実機の症状そのもの)。
        self.assertEqual(self.out[0], 'PARSE ok',
                         'esp_hid が記述子を読めない:\n' + self.err)

    def test_it_looks_like_a_keyboard(self):
        # 外観がキーボードでないと Mac のペアリング画面の絵が変わる。
        self.assertIn('APPEARANCE 0x03C1', self.out[1])

    def test_every_report_id_is_found(self):
        reports = [line for line in self.out if line.startswith('REPORT ')]
        found = {}
        for line in reports:
            parts = line.split()
            found.setdefault(int(parts[1]), []).append(parts[2])
        # Report ID 1 = キーボード (IN 8 バイト + OUT 1 バイト)、
        # 2 = マウス、3 = コンシューマ。
        self.assertIn(1, found)
        self.assertIn(2, found)
        self.assertIn(3, found)

    def reports(self):
        """(report_id, 種別, プロトコル) -> 長さ。"""
        out = {}
        for line in self.out:
            if not line.startswith('REPORT '):
                continue
            parts = dict(p.split('=') for p in line.split()[2:])
            out[(int(line.split()[1]), REPORT_TYPE[int(parts['type'])],
                 int(parts['proto']))] = int(parts['len'])
        return out

    def test_report_lengths_match_what_we_send(self):
        # protocol 1 = Report protocol (実際に使うほう)。
        # protocol 0 = Boot protocol。esp_hid は両方を別のレポートとして数える。
        lengths = self.reports()
        self.assertEqual(lengths[(1, 'INPUT', 1)], 8)   # mods+reserved+keys[6]
        self.assertEqual(lengths[(1, 'OUTPUT', 1)], 1)  # LED
        self.assertEqual(lengths[(2, 'INPUT', 1)], 5)   # buttons,x,y,v,h
        self.assertEqual(lengths[(3, 'INPUT', 1)], 2)   # consumer usage

    def test_report_count_fits_the_nimble_limit(self):
        """★ 2026-09-16 の実機で BLE が立ち上がらなかった原因。

        esp_hid が作るレポートの数が CONFIG_BT_NIMBLE_SVC_HID_MAX_RPTS を
        超えると、ble_svc_hid.c:725 が弾いて esp_hidd_dev_init() が
        ESP_FAIL を返し、**BLE が丸ごと動かない**。記述子を増やしたときに
        黙って踏まないよう、記述子から数え直して sdkconfig と突き合わせる。
        """
        import re
        count = len([line for line in self.out if line.startswith('REPORT ')])
        with open(os.path.join(IDF, 'sdkconfig.defaults'), encoding='utf-8') as h:
            text = h.read()
        match = re.search(r'^CONFIG_BT_NIMBLE_SVC_HID_MAX_RPTS=(\d+)', text,
                          flags=re.M)
        self.assertIsNotNone(match,
                             'sdkconfig.defaults に MAX_RPTS が無い '
                             '(既定の 3 では足りない)')
        limit = int(match.group(1))
        self.assertLessEqual(count, limit,
                             'レポートが %d 個。上限 %d を超えると BLE が'
                             '立ち上がらない' % (count, limit))


if __name__ == '__main__':
    unittest.main(verbosity=2)
