#!/usr/bin/env python3
"""Wi-Fi 自動接続の状態機械 (main/stackee_wifism.c) を Mac 上で確かめる。

**実機に触らない。** 無線も時計も偽物にして、**本物の状態機械**へ台本を流す。
見ているのは現行 CircuitPython 版 (stackee_wifi.py) と同じ約束:

  * 起動から 2 秒待ってから最初の探索。登録 0 件なら off で止まる
  * 走査は 1ch ずつ。1..11ch は 300 ms、12ch 以上は 800 ms 置いてから読む
  * チャネルの順番は「登録簿の ch → 前回のチャネル → CircuitPython の走査順」
  * 見えた中でいちばん強い登録済み AP へ繋ぐ。同じ強さなら登録順
  * 登録簿の channel が実際と違えば、走査で見えたほうを使う
  * **打鍵中は connect を撃たない**。30 秒粘っても谷が来なければ出直す
  * 録音・再生中は無線に触らない
  * 失敗したら無線を落として 60 秒後にやり直す
  * 繋がったら 10 秒おきに生存確認。切れたら 2 秒後にやり直す

  python3 firmware/tools/test_wifi_host.py
"""
import json
import os
import re
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
IDF = os.path.dirname(HERE)

SOURCES = ['stackee_wifism.c', 'stackee_wifistore.c', 'stackee_jsonlite.c']
# ★ ホストビルドは **ASan + UBSan つき**で回す。段階 3 の登録簿の直列化に
# 入れ物の外へ書く欠陥があり (snprintf の戻り値を足し込んでいた)、
# これを入れて初めて落ちるようになった。実機では同じ欠陥が静かに
# 隣の領域を壊す。遅くなるのは 1 秒未満。
SANITIZE = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']

_BINARY = None

PATTERN = [6, 1, 11, 3, 9, 13, 2, 4, 8, 12, 5, 7, 10, 14]


def binary():
    global _BINARY
    if _BINARY is None:
        out = os.path.join(tempfile.mkdtemp(), 'wifi')
        cmd = (['cc', '-O1', '-std=gnu11', '-Wall', '-Werror'] + SANITIZE + [
                '-I', os.path.join(IDF, 'main'), '-o', out,
                os.path.join(IDF, 'hostbuild/wifi_main.c')]
               + [os.path.join(IDF, 'main', s) for s in SOURCES])
        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError('ホストビルドに失敗:\n' + build.stderr)
        _BINARY = out
    return _BINARY


def run(script):
    out = subprocess.run([binary()], input=script, capture_output=True, text=True)
    if out.returncode != 0:
        raise AssertionError('実行に失敗:\n' + out.stderr)
    return out.stdout


def registry(nets):
    return json.dumps({'v': 1, 'networks': nets}, ensure_ascii=False)


def states(text):
    return [(name, int(ms)) for name, ms in
            re.findall(r'^STATE (\S+) (\d+)$', text, re.M)]


def scans(text):
    return [(int(ch), int(ms)) for ch, ms in
            re.findall(r'^SCAN (\d+) (\d+)$', text, re.M)]


def connects(text):
    return [(ssid, int(ch), int(ms)) for ssid, ch, ms in
            re.findall(r'^CONNECT (\S+) (-?\d+) (\d+)$', text, re.M)]


def last_print(text):
    rows = re.findall(
        r'^NOW (\d+) STATE (\S+) SSID (\S*) IP (\S*) NETS (\d+) PASSES (\d+) '
        r'CONNECTS (\d+) FAILURES (\d+) UP_MS (\d+) SCANNING (\d+)$', text, re.M)
    assert rows, text
    keys = ('now', 'state', 'ssid', 'ip', 'nets', 'passes', 'connects',
            'failures', 'up_ms', 'scanning')
    row = rows[-1]
    out = dict(zip(keys, row))
    for key in ('now', 'nets', 'passes', 'connects', 'failures', 'up_ms',
                'scanning'):
        out[key] = int(out[key])
    return out


HOME = {'ssid': 'home', 'password': 'homepass1', 'channel': 10}
CAFE = {'ssid': 'cafe', 'password': 'cafepass1', 'channel': None}


class BootTest(unittest.TestCase):
    def test_no_registry_means_off(self):
        out = run('nets %s\nt 3000\nprint\n' % registry([]))
        self.assertEqual(last_print(out)['state'], 'off')
        self.assertEqual(scans(out), [])
        self.assertNotIn('RADIO on', out)

    def test_first_scan_waits_two_seconds(self):
        out = run('nets %s\nap home 10 -50\nt 3000\nprint\n' % registry([HOME]))
        first = [ms for name, ms in states(out) if name == 'load'][0]
        self.assertGreaterEqual(first, 2000)
        self.assertLess(first, 2010)

    def test_connects_to_the_registered_ap(self):
        out = run('nets %s\nap home 10 -50\nt 3000\nprint\n' % registry([HOME]))
        info = last_print(out)
        self.assertEqual(info['state'], 'up')
        self.assertEqual(info['ssid'], 'home')
        self.assertEqual(info['ip'], '192.168.0.42')
        self.assertEqual(connects(out)[0][:2], ('home', 10))
        # 起動から接続までの時間が数字で残る (status に出す)。
        self.assertGreater(info['up_ms'], 2000)
        self.assertLess(info['up_ms'], 2600)


class ScanOrderTest(unittest.TestCase):
    def test_registered_channel_first(self):
        out = run('nets %s\nnoap\nt 20000\nprint\n'
                  % registry([{'ssid': 'x', 'password': 'p' * 8, 'channel': 4}]))
        order = [ch for ch, _ in scans(out)]
        want = [4] + [c for c in PATTERN if c != 4]
        self.assertEqual(order[:len(want)], want)

    def test_settle_times(self):
        out = run('nets %s\nnoap\nt 20000\nprint\n' % registry([CAFE]))
        rows = scans(out)
        for (ch, at), (_next_ch, next_at) in zip(rows, rows[1:]):
            want = 800 if ch >= 12 else 300
            # 走査開始 → settle → 読む → 次の走査開始 (数 ms の余地を見る)
            self.assertGreaterEqual(next_at - at, want)
            self.assertLessEqual(next_at - at, want + 5)

    def test_gives_up_after_one_pass_and_retries_in_60_seconds(self):
        out = run('nets %s\nnoap\nt 70000\nprint\n' % registry([CAFE]))
        self.assertIn('見つからない (14ch 走査)', out)
        self.assertIn('RADIO off', out)
        info = last_print(out)
        self.assertGreaterEqual(info['passes'], 2)
        self.assertGreaterEqual(info['failures'], 1)


class PickTest(unittest.TestCase):
    def test_strongest_wins(self):
        out = run('nets %s\nap cafe 6 -30\nap home 10 -70\nt 8000\nprint\n'
                  % registry([HOME, CAFE]))
        # 走査順は 10 (home の登録 ch) が先。だが 10ch では home しか見えず、
        # そこで繋いでしまう — これは現行 CircuitPython 版と同じ挙動。
        self.assertEqual(connects(out)[0][0], 'home')

    def test_scan_channel_beats_the_registry(self):
        # 登録簿は ch10 だが、実際は ch3 に居る。走査で見えたほうを使う。
        out = run('nets %s\nap home 3 -40\nt 8000\nprint\n' % registry([HOME]))
        self.assertIn('登録簿の ch=10 は実際と違う (走査で ch=3)', out)
        self.assertEqual(connects(out)[0][:2], ('home', 3))

    def test_unregistered_ap_is_ignored(self):
        out = run('nets %s\nap other 6 -20\nt 20000\nprint\n' % registry([HOME]))
        self.assertEqual(connects(out), [])
        self.assertEqual(last_print(out)['state'], 'wait')


class TypingTest(unittest.TestCase):
    def test_connect_waits_for_a_quiet_moment(self):
        out = run('nets %s\nap home 10 -50\nkeys 0\nt 5000\nkeys 1\nt 1000\nprint\n'
                  % registry([HOME]))
        at = connects(out)[0][2]
        self.assertGreater(at, 5000)
        self.assertEqual(last_print(out)['state'], 'up')

    def test_gives_up_after_thirty_seconds_of_typing(self):
        out = run('nets %s\nap home 10 -50\nkeys 0\nt 40000\nprint\n'
                  % registry([HOME]))
        self.assertIn('打鍵が続くので出直す', out)
        self.assertEqual(connects(out), [])
        self.assertIn('RADIO off', out)


class AudioTest(unittest.TestCase):
    def test_radio_is_not_touched_while_recording_or_playing(self):
        out = run('nets %s\nap home 10 -50\naudio 1\nt 10000\nprint\n'
                  % registry([HOME]))
        self.assertNotIn('RADIO on', out)
        self.assertEqual(scans(out), [])
        self.assertEqual(last_print(out)['state'], 'radio')

    def test_resumes_when_the_audio_stops(self):
        out = run('nets %s\nap home 10 -50\naudio 1\nt 5000\naudio 0\nt 3000\nprint\n'
                  % registry([HOME]))
        self.assertEqual(last_print(out)['state'], 'up')


class FailureTest(unittest.TestCase):
    def test_connect_failure_folds_the_radio(self):
        out = run('nets %s\nap home 10 -50\nreason 202\nt 8000\nprint\n'
                  % registry([HOME]))
        self.assertIn('接続失敗 ssid=home reason=202', out)
        self.assertIn('RADIO off', out)
        self.assertEqual(last_print(out)['state'], 'wait')

    def test_connect_timeout(self):
        out = run('nets %s\nap home 10 -50\nreason 0\nt 30000\nprint\n'
                  % registry([HOME]))
        self.assertIn('接続がタイムアウト ssid=home', out)
        self.assertEqual(last_print(out)['state'], 'wait')

    def test_start_failure(self):
        out = run('nets %s\nap home 10 -50\nconnect 0\nt 8000\nprint\n'
                  % registry([HOME]))
        self.assertIn('接続を始められない ssid=home', out)

    def test_password_never_appears_in_the_log(self):
        out = run('nets %s\nap home 10 -50\nreason 202\nt 8000\nprint\n'
                  % registry([HOME]))
        self.assertNotIn('homepass1', out)

    def test_link_loss_retries_in_two_seconds(self):
        out = run('nets %s\nap home 10 -50\nt 4000\ndrop\nt 20000\nprint\n'
                  % registry([HOME]))
        # 10 秒おきの生存確認で切れに気づき、2 秒後にやり直す。
        self.assertIn('接続が切れた。やり直す', out)
        self.assertGreaterEqual(last_print(out)['passes'], 2)


class ConsoleTest(unittest.TestCase):
    def test_kick_restarts_immediately(self):
        out = run('nets %s\nnoap\nt 3000\nap home 10 -50\nkick\nt 2000\nprint\n'
                  % registry([HOME]))
        self.assertIn('KICK load', out)
        self.assertEqual(last_print(out)['state'], 'up')

    def test_suspend_stops_scanning_and_resume_brings_it_back(self):
        out = run('nets %s\nap home 10 -50\nt 2050\nsuspend\nt 2000\nresume\n'
                  't 3000\nprint\n' % registry([HOME]))
        self.assertIn('SUSPEND 0', out)     # まだ繋がっていない
        self.assertEqual(last_print(out)['state'], 'up')

    def test_suspend_while_up_reports_that_the_link_is_held(self):
        out = run('nets %s\nap home 10 -50\nt 4000\nsuspend\nt 100\nprint\n'
                  % registry([HOME]))
        self.assertIn('SUSPEND 1', out)


if __name__ == '__main__':
    unittest.main(verbosity=2)
