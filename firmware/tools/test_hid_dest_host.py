#!/usr/bin/env python3
"""HID の送信先の選び方を、実機のコードそのままで確かめる。

    python3 firmware/tools/test_hid_dest_host.py

BLE のスタックは実機でしか試せないが、「どちらへ出すか」の判断は
ただのロジックなので Mac 上で走らせられる。確かめるのは現行
CircuitPython 版の決まりをそのまま持ってこられているか:

  * 既定は BLE (USB ケーブルを挿していても BLE)
  * STK_HID_SWITCH でトグル
  * 選択は NVS に残り、再起動後も維持される
  * USB を選んでいてもケーブルが無ければ BLE に倒す (選択は変えない)
"""
import os
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
IDF = os.path.dirname(HERE)

_BINARY = None


def build_once():
    global _BINARY
    if _BINARY:
        return _BINARY
    tmp = tempfile.mkdtemp(prefix='stackee-hid-dest-')
    binary = os.path.join(tmp, 'hid_dest')
    cmd = ['cc', '-O1', '-std=gnu11', '-Wall', '-Werror',
           '-I', os.path.join(IDF, 'main'),
           os.path.join(IDF, 'hostbuild', 'hid_dest_main.c'),
           os.path.join(IDF, 'main', 'stackee_hid_dest.c'),
           '-o', binary]
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode != 0:
        raise AssertionError('ホストビルドに失敗:\n' + done.stderr)
    _BINARY = binary
    return binary


def run(script):
    done = subprocess.run([build_once()], input=script,
                          capture_output=True, text=True)
    if done.returncode != 0:
        raise AssertionError('実行に失敗:\n' + done.stderr)
    return done.stdout.splitlines()


class DestinationTest(unittest.TestCase):
    def test_default_is_ble_even_with_usb_plugged_in(self):
        # ★ 現行の普段使い: ケーブルは充電のために挿さっているが、打鍵は BLE。
        self.assertEqual(run('usb 1\nshow\n'),
                         ['sel=BLE eff=BLE saves=0 saved=-'])

    def test_switch_toggles_and_is_saved(self):
        out = run('usb 1\ntoggle\nshow\ntoggle\nshow\n')
        self.assertEqual(out[0], 'sel=USB eff=USB saves=1 saved=USB')
        self.assertEqual(out[1], 'sel=BLE eff=BLE saves=2 saved=BLE')

    def test_selection_survives_a_restart(self):
        out = run('usb 1\ntoggle\ninit\nshow\n')
        self.assertEqual(out, ['sel=USB eff=USB saves=1 saved=USB'])

    def test_usb_without_a_cable_falls_back_to_ble(self):
        # USB を選んでいてもケーブルが無ければ BLE へ出す。
        out = run('usb 1\ntoggle\nusb 0\nshow\n')
        self.assertEqual(out, ['sel=USB eff=BLE saves=1 saved=USB'])

    def test_the_fallback_does_not_change_the_selection(self):
        # 挿し直せば黙って USB に戻る (選択を書き換えていない証拠)。
        out = run('usb 1\ntoggle\nusb 0\nshow\nusb 1\nshow\n')
        self.assertEqual(out[0], 'sel=USB eff=BLE saves=1 saved=USB')
        self.assertEqual(out[1], 'sel=USB eff=USB saves=1 saved=USB')

    def test_an_empty_nvs_means_ble(self):
        out = run('usb 1\ntoggle\nnvs clear\ninit\nshow\n')
        self.assertEqual(out, ['sel=BLE eff=BLE saves=0 saved=-'])

    def test_explicit_set_also_saves(self):
        out = run('usb 1\nset USB\nshow\nset BLE\nshow\n')
        self.assertEqual(out[0], 'sel=USB eff=USB saves=1 saved=USB')
        self.assertEqual(out[1], 'sel=BLE eff=BLE saves=2 saved=BLE')


if __name__ == '__main__':
    unittest.main(verbosity=2)
