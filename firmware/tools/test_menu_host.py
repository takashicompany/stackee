#!/usr/bin/env python3
"""本体の設定メニューのテスト。**実機に触らない。**

    python3 firmware/tools/test_menu_host.py

main/stackee_menu_core.c (階層・選択・パスワードの編集・ops の呼び出し) と
main/stackee_draw.c の stackee_draw_menu (画面) を Mac 用にビルドし、
台本 (hostbuild/menu_main.c) を流して確かめる。画面の CRC32 は
tools/menu_expected.py が同じ font16.bin から描いた期待値と突き合わせる。

キーの横取り (メニュー中のキーが PC へ出ないこと) は QMK ごと走らせる
tools/test_keyseq_host.py の MenuGateTest。
"""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
IDF = HERE.parent
sys.path.insert(0, str(HERE))
import gen_font16                                  # noqa: E402
import menu_expected                               # noqa: E402

FONT16 = IDF / 'assets/font16.bin'
SOURCES = ['stackee_menu_core.c', 'stackee_wifistore.c', 'stackee_jsonlite.c', 'stackee_font16.c',
           'stackee_draw.c', 'stackee_icons.c', 'stackee_bdf.c',
           'stackee_font8x8.c', 'stackee_crc32.c']
SANITIZE = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']

_BIN = None
_FONT = None


def binary():
    global _BIN
    if _BIN is None:
        out = Path(tempfile.mkdtemp(prefix='stackee-menu-')) / 'menu'
        cmd = (['cc', '-O1', '-std=gnu11', '-Wall', '-Werror'] + SANITIZE +
               ['-I', str(IDF / 'main'), '-o', str(out),
                str(IDF / 'hostbuild/menu_main.c')]
               + [str(IDF / 'main' / s) for s in SOURCES])
        done = subprocess.run(cmd, capture_output=True, text=True)
        if done.returncode != 0:
            raise AssertionError('ホストビルドに失敗:\n' + done.stderr)
        _BIN = out
    return _BIN


def font():
    global _FONT
    if _FONT is None:
        _FONT = gen_font16.load(FONT16)
    return _FONT


def run(script):
    done = subprocess.run([str(binary()), str(FONT16)], input=script,
                          capture_output=True, text=True)
    if done.returncode != 0:
        raise AssertionError('実行に失敗:\n' + done.stderr)
    return [l for l in done.stdout.splitlines() if l]


def views(lines):
    """VIEW と CRC の組を順に返す。"""
    out = []
    for i, line in enumerate(lines):
        if line.startswith('VIEW '):
            view = json.loads(line[5:])
            crc = int(lines[i + 1].split()[1])
            out.append((view, crc))
    return out


def last_view(lines):
    return views(lines)[-1][0]


def ops(lines):
    return [l for l in lines if l.startswith('OP ')]


def state(lines):
    got = [l for l in lines if l.startswith('STATE ')][-1]
    return dict(kv.split('=') for kv in got.split()[1:])


def labels(view):
    return [r[3] for r in view['rows']]


def selected_label(view):
    return view['rows'][view['selected']][3] if view['selected'] >= 0 else None


# 接続済みの本体の様子 (よくある状態)。
BASE = '\n'.join([
    'set wifi_state up', 'set ssid home', 'set ip 192.168.0.10', 'set rssi -52',
    'saved home,office',
    'set server_configured 1', 'set server_host pi400.example.ts.net:8443',
    'set http_seen 1', 'set http_last_ok 1', 'set http_last_ago_ms 12000',
    'set inbox_on 1', 'set inbox_phase wait',
    'set clips_ready 1', 'set clips_count 3', 'set clips_bytes 1572864',
    'set clips_free 8912896', 'set clips_auto 1', 'set clips_synced 1',
    'set clips_last_ago_ms 180000', 'set clips_result ok', 'set clips_phase idle',
    'set version 89c049e-dirty', 'set profile full', 'set dest_selected 0',
    'set dest_effective 0', 'set ble_connected 1', 'set ble_peer AA:BB:CC:DD:EE:FF',
    'set battery 85', 'set battery_present 1', 'set mic_ready 1', 'set volume 20',
    '']) + '\n'


class NavigationTest(unittest.TestCase):
    def test_root_lists_the_four_sections(self):
        v = last_view(run(BASE + 'open\nview\n'))
        self.assertEqual(v['screen'], 'root')
        self.assertEqual(v['title'], '設定')
        self.assertEqual(labels(v), ['Wi-Fi', 'サーバー', 'クリップ', '本体'])
        self.assertEqual(v['rows'][0][4], 'home')           # 接続中の SSID
        self.assertEqual(selected_label(v), 'Wi-Fi')
        self.assertIn('Esc閉じる', v['footer'])

    def test_arrows_move_and_stop_at_the_ends(self):
        lines = run(BASE + 'open\nkey up\nview\nkey down\nkey down\nkey down\n'
                    'key down\nview\n')
        (first, _), (last, _) = views(lines)
        self.assertEqual(selected_label(first), 'Wi-Fi')
        self.assertEqual(selected_label(last), '本体')

    def test_enter_goes_down_and_esc_or_backspace_come_back(self):
        lines = run(BASE + 'open\nkey down\nkey down\nkey down\nkey enter\nview\n'
                    'key esc\nview\nkey enter\nkey bs\nview\nstate\n')
        (dev, _), (back, _), (back2, _) = views(lines)
        self.assertEqual(dev['screen'], 'device')
        self.assertEqual(back['screen'], 'root')
        self.assertEqual(selected_label(back), '本体')     # 選んでいた所に戻る
        self.assertEqual(back2['screen'], 'root')
        self.assertEqual(state(lines)['depth'], '0')

    def test_esc_at_the_root_closes(self):
        lines = run(BASE + 'open\nkey esc\nstate\n')
        self.assertEqual(ops(lines), ['OP close'])
        self.assertEqual(state(lines)['open'], '0')

    def test_backspace_at_the_root_does_nothing(self):
        lines = run(BASE + 'open\nkey bs\nstate\n')
        self.assertEqual(ops(lines), [])
        self.assertEqual(state(lines)['open'], '1')

    def test_keys_are_ignored_when_closed(self):
        lines = run(BASE + 'key enter\nkey down\nstate\n')
        self.assertEqual(ops(lines), [])
        self.assertEqual(state(lines)['keys'], '0')

    def test_open_always_starts_at_the_root(self):
        lines = run(BASE + 'open\nkey enter\nclose\nopen\nview\n')
        self.assertEqual(last_view(lines)['screen'], 'root')


class WifiTest(unittest.TestCase):
    def test_status_rows(self):
        v = last_view(run(BASE + 'open\nkey enter\nview\n'))
        self.assertEqual(v['screen'], 'wifi')
        rows = {r[3]: r[4] for r in v['rows']}
        self.assertEqual(rows['状態'], '接続中')
        self.assertEqual(rows['SSID'], 'home')
        self.assertEqual(rows['電波'], '-52 dBm ■■■■')
        self.assertEqual(rows['IP'], '192.168.0.10')
        self.assertEqual(rows['登録済みネットワーク'], '2 件')

    def test_disconnected_shows_only_the_state(self):
        v = last_view(run(BASE + 'set wifi_state scan_wait\nset ssid -\n'
                          'open\nkey enter\nview\n'))
        rows = {r[3]: r[4] for r in v['rows']}
        self.assertEqual(rows['状態'], '探しています')
        self.assertEqual(rows['SSID'], '-')
        self.assertEqual(rows['電波'], '-')

    def test_switch_to_a_saved_network(self):
        lines = run(BASE + 'open\nkey enter\nkey enter\nview\nkey down\nkey enter\n'
                    'view\nset target office\nset target_result 1\nview\n'
                    'set target_result 2\nset ssid office\nview\nkey enter\nview\n')
        saved, result, trying, done, back = [v for v, _ in views(lines)]
        self.assertEqual(saved['screen'], 'wifi_saved')
        self.assertEqual([(r[3], r[4]) for r in saved['rows']],
                         [('home', '接続中'), ('office', '')])
        self.assertEqual(ops(lines), ['OP switch office'])
        self.assertEqual(result['screen'], 'wifi_result')
        self.assertIn('接続しています…', labels(trying))
        self.assertIn('接続しました', labels(done))
        self.assertEqual(back['screen'], 'wifi')           # 戻り先は Wi-Fi

    def test_switching_to_the_current_one_does_nothing(self):
        lines = run(BASE + 'open\nkey enter\nkey enter\nkey enter\nview\n')
        self.assertEqual(ops(lines), [])
        self.assertIn('もう接続しています', labels(last_view(lines)))

    def test_a_failed_switch_says_why(self):
        v = last_view(run(BASE + 'open\nkey enter\nkey enter\nkey down\nkey enter\n'
                          'set target office\nset target_result 3\n'
                          'set target_reason 202\nview\n'))
        self.assertIn('接続できませんでした', labels(v))
        self.assertIn(['理由', 'パスワード違い?'], [r[3:] for r in v['rows']])

    def test_add_a_network_with_a_password(self):
        script = (BASE + 'open\nkey enter\nkey down\nkey enter\nview\n'
                  'set scan_state 2\nnet cafe -60 6 1\nnet free -70 11 0\nview\n'
                  'key enter\nview\n'
                  'type abc\nkey bs\nstate\nkey enter\nview\n'
                  'type defghij\nstate\nview\nkey enter\nstate\nview\n')
        lines = run(script)
        vs = [v for v, _ in views(lines)]
        scanning, listed, pw, short, typed, result = vs
        self.assertEqual(ops(lines)[0], 'OP scan')
        self.assertIn('探しています…', labels(scanning))
        self.assertEqual([(r[3], r[4]) for r in listed['rows']][:2],
                         [('cafe', '■■■□ 鍵'), ('free', '■■□□ 開')])
        self.assertEqual(pw['screen'], 'wifi_password')
        states = [l for l in lines if l.startswith('STATE ')]
        self.assertIn('pass_len=2', states[0])              # abc → BS → ab
        self.assertIn('8〜63 文字にしてください', labels(short))
        self.assertIn('pass_len=9', states[1])
        # ★ 伏せ字だけ。打った文字は画面に出ない。
        self.assertIn('*********_', labels(typed))
        for line in lines:
            if line.startswith('VIEW '):
                self.assertNotIn('abdefghij', line)
        self.assertEqual(ops(lines)[-1], 'OP add cafe abdefghij 6')
        after = dict(kv.split('=') for kv in states[2].split()[1:])
        self.assertEqual(after['pass_len'], '0')
        self.assertEqual(after['pass_zero'], '1')           # 渡したら消す
        self.assertEqual(after['screen'], 'wifi_result')
        self.assertEqual(after['depth'], '2')               # 戻ると Wi-Fi
        self.assertEqual(result['rows'][0][4], 'cafe')

    def test_escape_cancels_the_password_and_wipes_it(self):
        lines = run(BASE + 'open\nkey enter\nkey down\nkey enter\nset scan_state 2\n'
                    'net cafe -60 6 1\nkey enter\ntype secret12\nkey esc\nstate\nview\n')
        st = state(lines)
        self.assertEqual(st['screen'], 'wifi_scan')
        self.assertEqual(st['pass_zero'], '1')
        self.assertFalse([o for o in ops(lines) if o.startswith('OP add')])

    def test_backspace_on_an_empty_password_stays(self):
        lines = run(BASE + 'open\nkey enter\nkey down\nkey enter\nset scan_state 2\n'
                    'net cafe -60 6 1\nkey enter\nkey bs\nkey bs\nstate\n')
        self.assertEqual(state(lines)['screen'], 'wifi_password')

    def test_closing_during_the_password_wipes_it(self):
        lines = run(BASE + 'open\nkey enter\nkey down\nkey enter\nset scan_state 2\n'
                    'net cafe -60 6 1\nkey enter\ntype secret12\nclose\nstate\n')
        self.assertEqual(state(lines)['pass_zero'], '1')

    def test_arrows_do_not_leave_the_password(self):
        lines = run(BASE + 'open\nkey enter\nkey down\nkey enter\nset scan_state 2\n'
                    'net cafe -60 6 1\nkey enter\nkey up\nkey down\nkey left\nstate\n')
        self.assertEqual(state(lines)['screen'], 'wifi_password')

    def test_an_open_network_is_added_without_a_password(self):
        lines = run(BASE + 'open\nkey enter\nkey down\nkey enter\nset scan_state 2\n'
                    'net free -70 11 0\nkey enter\nstate\n')
        self.assertEqual(ops(lines)[-1], 'OP add free (空) 11')
        self.assertEqual(state(lines)['screen'], 'wifi_result')

    def test_a_refused_add_stays_and_says_so(self):
        lines = run(BASE + 'fail add 1\nopen\nkey enter\nkey down\nkey enter\n'
                    'set scan_state 2\nnet cafe -60 6 1\nkey enter\ntype abcdefgh\n'
                    'key enter\nstate\nview\n')
        self.assertEqual(state(lines)['screen'], 'wifi_password')
        self.assertIn('いまは登録できません', labels(last_view(lines)))

    def test_a_failed_scan_offers_to_try_again(self):
        lines = run(BASE + 'open\nkey enter\nkey down\nkey enter\nset scan_state 3\n'
                    'set scan_error audio_busy\nview\nkey enter\n')
        v = last_view(lines)
        self.assertIn('探せません (audio_busy)', labels(v))
        self.assertEqual(ops(lines), ['OP scan', 'OP scan'])

    def test_delete_asks_first_and_defaults_to_no(self):
        lines = run(BASE + 'open\nkey enter\nkey down\nkey down\nkey enter\n'
                    'key down\nkey enter\nview\nkey enter\nview\n'
                    'key enter\nkey up\nkey enter\nview\n')
        confirm, cancelled, deleted = [v for v, _ in views(lines)]
        self.assertEqual(confirm['screen'], 'wifi_confirm')
        self.assertEqual(selected_label(confirm), 'やめる')
        self.assertEqual(cancelled['screen'], 'wifi_delete')
        self.assertEqual(ops(lines), ['OP remove office'])
        self.assertIn('削除しました: office', labels(deleted))


class OtherScreensTest(unittest.TestCase):
    def test_server(self):
        lines = run(BASE + 'open\nkey down\nkey enter\nview\nkey enter\n'
                    'set health_state 1\nview\nkey enter\nset health_state 2\n'
                    'set health_ms 321\nview\n')
        first, running, done = [v for v, _ in views(lines)]
        rows = {r[3]: r[4] for r in first['rows']}
        self.assertIn('pi400.example.ts.net:8443', rows)    # 1 行ぶん使う
        self.assertEqual(rows['直近の通信'], '成功 12秒前')
        self.assertEqual(rows['受け箱'], '待っています')
        self.assertEqual(ops(lines), ['OP health'])        # テスト中は撃ち直さない
        self.assertEqual(running['rows'][running['selected']][4], 'テスト中…')
        self.assertEqual(done['rows'][done['selected']][4], 'OK 321 ms')

    def test_server_holds_the_inbox_while_open(self):
        v = last_view(run(BASE + 'set inbox_held 1\nopen\nkey down\nkey enter\nview\n'))
        self.assertEqual({r[3]: r[4] for r in v['rows']}['受け箱'], '保留中 (メニュー中)')

    def test_server_not_configured(self):
        v = last_view(run(BASE + 'set server_configured 0\nopen\nkey down\n'
                          'key enter\nview\n'))
        self.assertEqual(labels(v), ['STACKEE_TALK_URL 未設定'])

    def test_clips(self):
        lines = run(BASE + 'open\nkey down\nkey down\nkey enter\nview\n'
                    'key enter\nkey down\nkey enter\n')
        v = views(lines)[0][0]
        rows = {r[3]: r[4] for r in v['rows']}
        self.assertEqual(rows['件数'], '3 件')
        self.assertEqual(rows['合計'], '1.5 MB')
        self.assertEqual(rows['FAT の空き'], '8.5 MB')
        self.assertEqual(rows['最後の取り込み'], '3分前')
        self.assertEqual(rows['結果'], '成功')
        self.assertEqual(rows['自動取得'], 'ON')
        self.assertEqual(ops(lines), ['OP clip_auto', 'OP clip_sync'])

    def test_clip_auto_forced_off_by_settings(self):
        lines = run(BASE + 'set clips_forced_off 1\nopen\nkey down\nkey down\n'
                    'key enter\nkey enter\nview\n')
        self.assertEqual(ops(lines), [])
        v = last_view(lines)
        self.assertIn('settings.toml で OFF です', labels(v))
        self.assertIn(['自動取得', 'OFF (設定)'], [r[3:] for r in v['rows']])

    def test_device(self):
        lines = run(BASE + 'open\nkey down\nkey down\nkey down\nkey enter\nview\n'
                    'key enter\nkey down\nkey left\nkey right\nkey right\nkey enter\nview\n')
        v = views(lines)[0][0]
        rows = {r[3]: r[4] for r in v['rows']}
        self.assertEqual(rows['送信先'], 'BLE')
        self.assertEqual(rows['Bluetooth'], 'AA:BB:CC:DD:EE:FF')
        self.assertEqual(rows['電池'], '85%')
        self.assertEqual(rows['USB マイク'], '使える')
        self.assertEqual(rows['音量'], '← 20% →')
        self.assertEqual(ops(lines), ['OP hid_toggle', 'OP volume -5',
                                      'OP volume 5', 'OP volume 5'])
        self.assertIn('← → で変えます', labels(last_view(lines)))

    def test_device_without_battery_and_usb_fallback(self):
        v = last_view(run(BASE + 'set battery_present 0\nset dest_selected 1\n'
                          'set dest_effective 0\nset ble_connected 0\n'
                          'set ble_advertising 1\nopen\nkey down\nkey down\nkey down\n'
                          'key enter\nview\n'))
        rows = {r[3]: r[4] for r in v['rows']}
        self.assertEqual(rows['電池'], 'なし')
        self.assertEqual(rows['送信先'], 'USB (未接続→BLE)')
        self.assertEqual(rows['Bluetooth'], '待ち受け中')


class HidMappingTest(unittest.TestCase):
    CASES = [
        ('04 00', 'char', 'a'), ('04 02', 'char', 'A'), ('1D 20', 'char', 'Z'),
        ('1E 00', 'char', '1'), ('1F 02', 'char', '"'), ('24 02', 'char', "'"),
        ('2D 02', 'char', '='), ('2E 00', 'char', '^'), ('2E 02', 'char', '~'),
        ('2F 00', 'char', '@'), ('2F 02', 'char', '`'), ('30 02', 'char', '{'),
        ('32 00', 'char', ']'), ('33 02', 'char', '+'), ('34 00', 'char', ':'),
        ('34 02', 'char', '*'), ('38 02', 'char', '?'), ('87 00', 'char', '\\'),
        ('87 02', 'char', '_'), ('89 02', 'char', '|'), ('2C 00', 'char', ' '),
        ('52 00', 'up', None), ('51 00', 'down', None), ('50 00', 'left', None),
        ('4F 00', 'right', None), ('28 00', 'enter', None), ('29 00', 'esc', None),
        ('2A 00', 'bs', None), ('4C 00', 'bs', None), ('2B 00', 'down', None),
        ('2B 02', 'up', None),
    ]

    def test_jis_layout(self):
        script = ''.join('hid %s\n' % c for c, _k, _ch in self.CASES)
        got = [l for l in run(script) if l.startswith('HIDKEY ')]
        for (code, key, ch), line in zip(self.CASES, got):
            _, name, value = line.split(' ')
            self.assertEqual(name, key, code)
            if ch is not None:
                self.assertEqual(chr(int(value)), ch, code)

    def test_unused_keys(self):
        # Shift+0 は JIS で何も出ない。Ctrl / Cmd つきの文字は使わない。F24 も。
        got = [l for l in run('hid 27 02\nhid 04 01\nhid 04 08\nhid 73 00\n')
               if l.startswith('HIDKEY ')]
        self.assertEqual(got, ['HIDKEY - 0'] * 4)

    def test_typing_through_hid_reaches_the_password(self):
        lines = run(BASE + 'open\nkey enter\nkey down\nkey enter\nset scan_state 2\n'
                    'net cafe -60 6 1\nkey enter\n'
                    'hid 04 02\nhid 05 00\nhid 1F 02\nhid 34 02\nhid 2F 00\n'
                    'hid 1E 00\nhid 1F 00\nhid 20 00\nhid 28 00\n')
        self.assertEqual(ops(lines)[-1], 'OP add cafe Ab"*@123 6')


class RenderTest(unittest.TestCase):
    """C の描画と Python の期待値 (tools/menu_expected.py) が同じ絵か。"""

    SCRIPTS = [
        'open\nview\n',
        'open\nkey enter\nview\n',
        'open\nkey enter\nkey enter\nview\n',
        'open\nkey enter\nkey down\nkey enter\nset scan_state 2\n'
        'net cafe -60 6 1\nnet a-very-long-ssid-name-that-overflows -80 1 1\nview\n'
        'key enter\ntype abcdefghij\nview\n',
        'open\nkey down\nkey enter\nview\n',
        'open\nkey down\nkey down\nkey enter\nview\n',
        'open\nkey down\nkey down\nkey down\nkey enter\nkey down\nview\n',
        'set server_configured 0\nset clips_ready 0\nopen\nview\n',
    ]

    def test_every_screen_matches_the_reference(self):
        checked = 0
        for script in self.SCRIPTS:
            for view, crc in views(run(BASE + script)):
                self.assertEqual(crc, menu_expected.expected_crc(view, font()),
                                 json.dumps(view, ensure_ascii=False))
                checked += 1
        self.assertGreaterEqual(checked, 9)

    def test_scrolling_keeps_the_selection_visible(self):
        nets = ''.join('net net%02d -%d 1 1\n' % (i, 50 + i) for i in range(12))
        lines = run(BASE + 'open\nkey enter\nkey down\nkey enter\nset scan_state 2\n'
                    + nets + 'view\n' + 'key down\n' * 12 + 'view\n')
        (first, c1), (last, c2) = views(lines)
        self.assertEqual(first['top'], 0)
        self.assertEqual(selected_label(last), 'もう一度探す')
        self.assertEqual(last['top'], last['selected'] - 10)
        self.assertEqual(c1, menu_expected.expected_crc(first, font()))
        self.assertEqual(c2, menu_expected.expected_crc(last, font()))

    def test_every_fixed_text_fits_on_one_line(self):
        """決まり文句 (題・足もと・見出し・お知らせ) は切れずに 1 行に収まる。

        SSID・ホスト名・版などの外から来る文字列だけは長ければ切れる (はみ出す
        字は描かない)。
        """
        f = font()
        width = 240 - 2 * 6
        extra = ['set job_state 3\nset job_error write_failed\nopen\nkey enter\nview\n',
                 'open\nkey enter\nkey down\nkey enter\nset scan_state 3\n'
                 'set scan_error audio_busy\nview\n',
                 'open\nkey enter\nkey enter\nkey down\nkey enter\nset target office\n'
                 'set target_result 3\nset target_reason 205\nview\n',
                 'set health_state 3\nset health_status 404\nset health_error 通信に失敗しました\n'
                 'open\nkey down\nkey enter\nview\n',
                 'set clips_forced_off 1\nset clips_synced 1\nset clips_last_ago_ms 43200000\n'
                 'set clips_result same\nopen\nkey down\nkey down\nkey enter\nkey enter\nview\n',
                 'set dest_selected 1\nset dest_effective 0\nset charging 1\n'
                 'open\nkey down\nkey down\nkey down\nkey enter\nkey enter\nview\n',
                 'open\nkey enter\nkey down\nkey down\nkey enter\nkey down\nkey enter\nview\n']
        user = ('pi400', 'cafe', 'a-very', '89c049e', 'AA:BB', 'home', 'office', 'net')
        checked = 0
        for script in self.SCRIPTS + extra:
            for view, _crc in views(run(BASE + script)):
                for text in (view['title'], view['footer']):
                    self.assertLessEqual(f.text_px(text), width, text)
                for kind, _a, _arg, label, value in view['rows']:
                    if any(u in label or u in value for u in user):
                        continue
                    need = f.text_px(label) + (8 + f.text_px(value) if value else 0)
                    limit = width - (20 if kind == 2 else 0)
                    self.assertLessEqual(need, limit, (label, value))
                    checked += 1
        self.assertGreater(checked, 40)

    def test_the_menu_paints_every_pixel_of_its_region(self):
        # menu_main.c は描く前に 0x5A で埋める。描き残しがあれば期待値と合わない。
        # 念のため「白と黒と灰色の帯」が本当に入っているかも見る。
        view, crc = views(run(BASE + 'open\nview\n'))[0]
        region = menu_expected.render(font(), view)
        self.assertEqual(crc, region.crc())
        self.assertNotIn(b'\x5A\x5A\x5A\x5A', bytes(region.buf))


if __name__ == '__main__':
    unittest.main(verbosity=2)
