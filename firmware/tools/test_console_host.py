#!/usr/bin/env python3
"""ファームの C をそのまま Mac 上で走らせて、出力を実測で確かめる。

実機には一切触らない。確かめるのは 2 つ:

  1. stackee_console.c が組み立てた枠を、**ホスト側の実物**
     (firmware/kmk/tools/stackee_console_client.py の FrameParser) が
     そのまま読めるか。読めた JSON に段階 0 で要る項目が揃っているか。
  2. stackee_assets.c の目録の拾い読みが、**本物の**
     firmware/kmk/stackee_assets/manifest.json に対して正しい数を出すか。

土台は firmware/hostbuild/ (ESP-IDF と TinyUSB の最小の代役)。
ここで通っても実機で動く保証にはならない — 見ているのは文字列の
組み立てと拾い読みだけ。

  python3 firmware/tools/test_console_host.py
  python3 -m pytest firmware/tools/test_console_host.py
"""
import json
import subprocess
import sys
import tempfile
import json
import unittest
from pathlib import Path

IDF = Path(__file__).resolve().parents[1]
HOSTBUILD = IDF / 'hostbuild'
MAIN = IDF / 'main'
sys.path.insert(0, str(IDF / 'tools'))
import stackee_tree as tree                     # noqa: E402

if not tree.add_kmk_tools(sys.path):
    tree.skip_module('ホスト側の枠の実物 (firmware/kmk/tools) が無いので読み解けない',
                     __name__)


def compile_and_run(source, extra_sources=(), args=(), extra_include=None):
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / 'a.out'
        cmd = ['cc', '-O1', '-Wall', '-Werror', '-Wno-unused-function',
               '-o', str(binary), str(source)]
        cmd += [str(s) for s in extra_sources]
        cmd += ['-I', str(HOSTBUILD / 'stub'), '-I', str(MAIN)]
        if extra_include:
            cmd += ['-I', str(extra_include)]
        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError('ホストビルドに失敗:\n' + build.stderr)
        run = subprocess.run([str(binary)] + [str(a) for a in args],
                             capture_output=True)
        if run.returncode != 0:
            raise AssertionError('実行に失敗:\n' + run.stderr.decode('utf-8', 'replace'))
        return run.stdout


class ConsoleOutputTest(unittest.TestCase):
    """C が吐いた枠を、ホスト側の実装で読み解く。"""

    @classmethod
    def setUpClass(cls):
        import stackee_console_client as ccl
        raw = compile_and_run(HOSTBUILD / 'console_main.c',
                              extra_sources=[MAIN / 'stackee_perf.c',
                                             MAIN / 'stackee_report_queue.c',
                                             MAIN / 'stackee_hid_dest.c',
                                             MAIN / 'stackee_logbuf.c',
                                             MAIN / 'stackee_jsonlite.c',
                                             # --- 段階 4 -----------------
                                             MAIN / 'stackee_settings.c',
                                             MAIN / 'stackee_conhid.c',
                                             HOSTBUILD / 'stub' / 'esp_log_stub.c'])
        frames, logs = ccl.FrameParser().feed(raw)
        cls.frames = frames
        cls.logs = logs

    def test_strings_survive_escaping(self):
        """`\\uXXXX` と `\\"` と `\\\\` を戻せるか。

        `stackee_console_client.py` は `ensure_ascii=True` で送るので、
        日本語の SSID は `\\u3042` の形で届く。パスワードに `"` や `\\` が
        入ることもある。ここを取りこぼすと wifi.add が静かに壊れる。
        """
        frame = [f for f in self.frames if f.get('id') == 99][0]
        self.assertEqual(frame['ssid'], 'あい')
        self.assertEqual(frame['password'], 'a"b\\c')

    def test_every_reply_is_a_readable_frame(self):
        # hello / status / log.tail / key.inject x4 / 文字列の取り出し /
        # unsupported / badjson / status(目録が読めない) の 11 個。
        self.assertEqual(len(self.frames), 25)
        self.assertEqual(self.logs, '', '枠の外に漏れている')

    def test_hello_matches_the_current_protocol(self):
        hello = self.frames[0]
        self.assertEqual(hello['id'], 7)
        self.assertEqual(hello['proto'], 2)
        self.assertEqual(hello['fw'], 'stackee-idf/5')
        self.assertIn('status', hello['features'])
        # 段階 1 で足した脱出路 (README の「戻し方」で使う)。
        self.assertIn('reset', hello['features'])
        self.assertIn('bootloader', hello['features'])
        # 段階 1b で足した BLE まわり。
        self.assertIn('hid.switch', hello['features'])
        self.assertIn('ble.refresh', hello['features'])

    def test_status_has_every_field_phase0_promises(self):
        status = self.frames[1]
        for key in ('fw', 'up', 'hid', 'ble', 'bat', 'wifi', 'volume',
                    'heap_free', 'perf', 'keys', 'hidq',
                    'hid_sel', 'ble_interval_ms', 'blex',
                    # 段階 3 で増えた分。Web 操作盤と check_phase3.py が読む。
                    'wifi_state', 'ssid', 'ip', 'nets', 'connect_ms',
                    'wifi_up_ms', 'volume_save_pending', 'volume_src',
                    'talk', 'audio_null', 'audio_busy', 'talk_url',
                    'talk_token', 'screen'):
            self.assertIn(key, status, '%s が無い' % key)
        self.assertEqual(status['fw'], 'stackee-idf/5')
        # 送信先の既定は BLE (現行 CircuitPython 版と同じ)。
        self.assertEqual(status['hid'], 'BLE')
        self.assertEqual(status['hid_sel'], 'BLE')
        self.assertEqual(status['ble'], True)        # 代役の BLE は接続ずみ
        self.assertEqual(status['ble_interval_ms'], 15)
        self.assertEqual(status['wifi'], 'up')      # 代役の Wi-Fi は接続ずみ
        self.assertEqual(status['wifi_state'], 'up')
        self.assertEqual(status['ip'], '192.168.0.42')
        self.assertEqual(status['volume'], 20)      # NVS に無いときの既定
        self.assertEqual(status['volume_src'], 'nvs')
        self.assertEqual(status['talk'], 'idle')
        # ★ 返答文は任意の日本語で、引用符も入りうる。JSON として壊れないこと
        #   (ここまで来ていれば json.loads が通っている)。
        self.assertEqual(status['screen'], 'こんにちは "なのだ"')
        self.assertEqual(status['bat'], 77)         # 代役の AXP2101
        self.assertEqual(status['up'], 12.3)

    def test_perf_carries_the_main_loop_period(self):
        main = self.frames[1]['perf']['main']
        # 1000..1299 us を 300 回入れたが、窓は直近 256 個なので
        # 1044..1299 が残り、中央値は 1044 + 128 = 1172。
        self.assertEqual(main['n'], 300)
        self.assertEqual(main['max_us'], 1299)
        self.assertEqual(main['med_us'], 1172)

    def test_log_tail_returns_the_boot_log_as_valid_json(self):
        # ★ 起動直後のログは CDC が繋がる前に流れる。あとから読めることが
        #   段階 1b 以降の検証の前提 (2026-09-16 に BLE の原因が読めなかった)。
        tail = self.frames[2]
        self.assertEqual(tail['id'], 11)
        self.assertGreater(tail['held'], 0)
        self.assertIn('起動ログ "1"', tail['text'])
        self.assertIn('BLE 失敗', tail['text'])
        # 改行やタブが JSON を壊していない (壊れていれば feed が読めない)。
        self.assertIn('\n', tail['text'])

    def test_key_inject_reports_the_latency_and_destination(self):
        # ★ 人手ゼロで打鍵を確かめるための口 (段階 1 の締め)。
        out = self.frames[3]
        self.assertEqual(out['id'], 12)
        self.assertEqual(out['ok'], 1)
        self.assertEqual(out['kc'], 0x73)        # 既定は F24 (ホストで無害)
        self.assertEqual(out['dest'], 'BLE')
        self.assertEqual(out['press_ms'], 1.234)
        self.assertEqual(out['release_ms'], 32.345)
        self.assertEqual(out['pushed'], 2)
        self.assertEqual(out['sent_ble'], 2)

    def test_key_inject_accepts_a_key_name_and_hold_time(self):
        out = self.frames[4]
        self.assertEqual(out['kc'], 0x90)        # LANG1 (JIS)
        self.assertEqual(out['hold_ms'], 50)

    def test_key_inject_can_start_without_waiting(self):
        """`"wait":false` は押し始めてすぐ返る (遅延は返らない)。

        ★ これが無いと、押している最中に `ui.status` や `lcd.crc` を
          読めない (コンソールのタスクが hold_ms のあいだ止まるため)。
          STK_MIC_KEY の表情を実機で確かめるのに要る。
        """
        frame = [f for f in self.frames if f.get('id') == 15][0]
        self.assertEqual(frame['ok'], 1)
        self.assertIs(frame['started'], True)
        self.assertIs(frame['wait'], False)
        self.assertEqual(frame['kc'], 0x7E08)       # STK_MIC_KEY
        self.assertEqual(frame['hold_ms'], 1500)
        # 遅延は返さない (測るなら既定の待つほうを使う)。
        self.assertNotIn('press_ms', frame)
        self.assertNotIn('release_ms', frame)

    def test_key_inject_refuses_an_unknown_name(self):
        self.assertEqual(self.frames[5],
                         {'id': 14, 'error': 'badkeycode'})

    def test_unknown_command_and_broken_json(self):
        # ★ 位置ではなく id で引く (段階 4 で枠が増えたときに、この検査だけ
        #   黙ってずれて別の枠を見ていた)。
        by_id = dict((f['id'], f) for f in self.frames
                     if isinstance(f.get('id'), int))
        self.assertEqual(by_id[9], {'id': 9, 'error': 'unsupported'})
        self.assertIn({'id': None, 'error': 'badjson'}, self.frames)

    def test_unreadable_manifest_is_reported_not_hidden(self):
        assets = self.frames[-1]['assets']
        self.assertFalse(assets['manifest'])
        self.assertTrue(assets['error'])

    def test_check_phase0_calls_it_a_pass(self):
        import check_phase0
        result = {'hello': self.frames[0], 'status': self.frames[1],
                  'reconnect': {'status_s': 2.1}}
        for name, ok, detail in check_phase0.verdicts(result):
            self.assertTrue(ok, '%s: %s' % (name, detail))


class ManifestParseTest(unittest.TestCase):
    """本物の manifest.json で、目録の拾い読みを確かめる。"""

    @classmethod
    def setUpClass(cls):
        # stackee_assets.c から拾い読みの部分だけを切り出す (実機側と同じ実体)。
        source = (MAIN / 'stackee_assets.c').read_text(encoding='utf-8')
        start = source.index('static const char *find_key')
        end = source.index('esp_err_t stackee_assets_mount')
        cls.tmp = tempfile.TemporaryDirectory()
        inc = Path(cls.tmp.name) / 'manifest_parse.inc'
        inc.write_text(source[start:end], encoding='utf-8')
        manifest = tree.asset('manifest.json')
        cls.out = compile_and_run(HOSTBUILD / 'manifest_main.c',
                                  args=[manifest],
                                  extra_include=Path(cls.tmp.name)
                                  ).decode('utf-8')

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_real_manifest(self):
        # 顔は 35 枚 (manifest の cases が 0..34 を指す)、1 辺 240 px。
        self.assertIn('v=1 size=240 faces=35 acks=5', self.out)

    def test_edge_cases(self):
        self.assertIn('empty=[0] one=[1] nested=[2] missing=[-1]', self.out)
        self.assertIn('comma-in-string=[2]', self.out)


class Phase4CommandTest(unittest.TestCase):
    """段階 4 で足したコマンドの応答の形。"""

    @classmethod
    def setUpClass(cls):
        cls.by_id = dict((f['id'], f) for f in ConsoleOutputTest.frames
                         if isinstance(f.get('id'), int))

    def test_settings_get_hides_secret_values(self):
        got = self.by_id[20]
        self.assertEqual('192.168.0.5', got['keys']['STACKEE_HOST'])
        # ★ パスワードとトークンは真偽だけ。値は 1 文字も出さない。
        self.assertIs(True, got['keys']['STACKEE_WIFI_PASSWORD'])
        self.assertIs(True, got['keys']['STACKEE_TALK_TOKEN'])
        self.assertNotIn('himitsu', json.dumps(got))
        self.assertNotIn('abcdef', json.dumps(got))
        self.assertIn('STACKEE_WIFI_PASSWORD', got['secret'])
        self.assertIn('STACKEE_HOST', got['allowed'])
        # ★ 書かせてはいけないキーが allowed に混ざっていないこと。
        self.assertNotIn('CIRCUITPY_WIFI_SSID', got['allowed'])

    def test_settings_raw_masks_the_password_lines(self):
        got = self.by_id[21]
        self.assertNotIn('himitsu', got['text'])
        self.assertNotIn('abcdef', got['text'])
        self.assertIn('STACKEE_WIFI_PASSWORD = "***"', got['text'])
        self.assertIn('STACKEE_HOST = "192.168.0.5"', got['text'])
        self.assertFalse(got['truncated'])

    def test_settings_set_writes_only_allowed_keys(self):
        self.assertEqual(1, self.by_id[22]['ok'])
        self.assertEqual(1, self.by_id[22]['changed'])
        self.assertEqual('denied:CIRCUITPY_WIFI_SSID', self.by_id[23]['error'])
        self.assertEqual('nokv', self.by_id[24]['error'])

    def test_bench_returns_numbers(self):
        got = self.by_id[25]
        self.assertEqual(5, got['n'])
        for key in ('in_waiting_ns', 'idle_poll_ns', 'main_med_us'):
            self.assertIsInstance(got[key], int)

    def test_log_burst_counts_the_bytes(self):
        got = self.by_id[26]
        self.assertEqual(3, got['n'])
        self.assertGreater(got['burst_bytes'], 0)
        self.assertIn('hid_tx_pending', got)

    def test_lcd_status_and_full(self):
        self.assertTrue(self.by_id[27]['ready'])
        self.assertEqual(240, self.by_id[27]['width'])
        self.assertEqual(320, self.by_id[27]['height'])
        self.assertEqual(1, self.by_id[28]['ok'])
        self.assertEqual(3, len(self.by_id[28]['before']))

    def test_usb_status(self):
        got = self.by_id[29]
        self.assertEqual('dev', got['profile'])
        self.assertTrue(got['cdc'])
        self.assertEqual(1, got['conhid']['proto'])
        self.assertFalse(got['uac']['enabled'])

    def test_loop_commands_say_unsupported(self):
        got = self.by_id[30]
        self.assertEqual('unsupported', got['error'])
        self.assertIn('perf', got['note'])

    def test_fs_put_assembles_then_writes(self):
        self.assertEqual(3, self.by_id[31]['have'])
        self.assertEqual(1, self.by_id[32]['ok'])
        self.assertEqual(5, self.by_id[32]['bytes'])
        self.assertEqual('stackee_assets/x.bin', self.by_id[32]['path'])
        # 続きを外れた位置から送ったら拒む (黙って詰め直さない)。
        self.assertEqual('nostart', self.by_id[33]['error'])

    def test_hello_lists_the_new_commands(self):
        features = ConsoleOutputTest.frames[0]['features']
        for name in ('settings.get', 'settings.raw', 'settings.set', 'fs.put',
                     'bench', 'log.burst', 'lcd.status', 'lcd.full',
                     'usb.status', 'touch.status', 'touch.inject',
                     'camera.capture', 'camera.power', 'camera.dump',
                     'camera.look', 'camera.look_status',
                     'key.cstm', 'key.cstm_status'):
            self.assertIn(name, features, name)

    def test_hello_says_which_profile(self):
        hello = ConsoleOutputTest.frames[0]
        self.assertEqual('dev', hello['profile'])
        self.assertTrue(hello['cdc'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
