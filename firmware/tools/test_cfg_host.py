#!/usr/bin/env python3
"""段階 3 の「設定・登録簿・音量」を Mac 上で突き合わせる。**実機には触らない。**

確かめたいのは 1 つだけ:

> **C 版が、現行 CircuitPython 版とまったく同じ答えを出すか。**

だから期待値は手で書かない。`firmware/kmk/stackee_wifi_store.py` と
`firmware/kmk/stackee_console.py` (settings.toml の読み) を **そのまま
import** して、同じ入力を C 版 (hostbuild/cfg_main.c から本物の
main/stackee_*.c をビルドしたもの) に流し、答えを 1 文字ずつ比べる。
音量のレジスタ値は `firmware/kmk/stackee_speaker.py` の `volume_bits` を
ソースから取り出して比べる (あのファイルは実機の import を含むので
モジュールごとは読み込めない)。

  python3 firmware/tools/test_cfg_host.py
  python3 -m pytest firmware/tools/test_cfg_host.py
"""
import importlib.util
import json
import math
import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
IDF = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import stackee_tree as tree             # noqa: E402

# 期待値は移植元 (現行 CircuitPython 版) をそのまま import して出す。
# 非公開側にしか無いので、無ければこのファイルは丸ごと飛ばす。
if tree.KMK is None:
    raise unittest.SkipTest(
        '移植元 (firmware/kmk) が無いので突き合わせられない')
KMK = str(tree.KMK)

SOURCES = ['stackee_jsonlite.c', 'stackee_settings.c', 'stackee_wifistore.c',
           'stackee_talksm.c', 'stackee_volume_core.c']

# ★ ホストビルドは **ASan + UBSan つき**で回す。段階 3 の登録簿の直列化に
# 入れ物の外へ書く欠陥があり (snprintf の戻り値を足し込んでいた)、
# これを入れて初めて落ちるようになった。実機では同じ欠陥が静かに
# 隣の領域を壊す。遅くなるのは 1 秒未満。
SANITIZE = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']

_BINARY = None


def binary():
    global _BINARY
    if _BINARY is None:
        out = os.path.join(tempfile.mkdtemp(), 'cfg')
        cmd = (['cc', '-O1', '-std=gnu11', '-Wall', '-Werror'] + SANITIZE + [
                '-I', os.path.join(IDF, 'main'), '-o', out,
                os.path.join(IDF, 'hostbuild/cfg_main.c')]
               + [os.path.join(IDF, 'main', s) for s in SOURCES])
        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError('ホストビルドに失敗:\n' + build.stderr)
        _BINARY = out
    return _BINARY


def run(args, text=''):
    out = subprocess.run([binary()] + list(args), input=text,
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise AssertionError('実行に失敗 (%s):\n%s' % (args, out.stderr))
    rows = []
    for line in out.stdout.splitlines():
        rows.append(line.split('\t'))
    return rows


def unescape(text):
    """cfg_main.c の settings が出す逃がしを戻す。"""
    out = []
    i = 0
    while i < len(text):
        if text[i] == '\\' and i + 1 < len(text):
            nxt = text[i + 1]
            out.append({'t': '\t', 'n': '\n', 'r': '\r'}.get(nxt, nxt))
            i += 2
            continue
        out.append(text[i])
        i += 1
    return ''.join(out)


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


STORE = load_module('kmk_wifi_store', os.path.join(KMK, 'stackee_wifi_store.py'))
CONSOLE = load_module('kmk_console', os.path.join(KMK, 'stackee_console.py'))


def kmk_volume_bits():
    """stackee_speaker.py の volume_bits をソースから取り出す。

    あのファイルは audiobusio / board を import するので、モジュールごとは
    CPython で読み込めない。関数の本文だけを切り出して動かす。
    """
    source = open(os.path.join(KMK, 'stackee_speaker.py')).read()
    match = re.search(r'^def volume_bits\(percent\):\n(?:    .*\n|\n)*',
                      source, re.M)
    assert match, 'stackee_speaker.py に volume_bits が見つからない'
    scope = {'math': math}
    exec(match.group(0), scope)
    return scope['volume_bits']


VOLUME_BITS = kmk_volume_bits()


class SettingsTest(unittest.TestCase):
    """settings.toml の読み方が CircuitPython 版と同じか。"""

    SAMPLES = [
        'STACKEE_TALK_URL = "https://pi400.example.ts.net:8443/talk"\n'
        'STACKEE_TALK_TOKEN = "abcdefghijklmnopqrstuvwxyz012345"\n',
        '# コメント\n'
        '  STACKEE_HOST="192.168.0.5"\n'
        'STACKEE_PORT = 5555\n'
        'BAD KEY = 1\n'
        '1BAD = 2\n'
        'EMPTY =\n',
        'A = "tab\\there"\nB = "nl\\nhere"\nC = "quote\\"x"\nD = plain\n',
        'DUP = "one"\nDUP = "two"\n',
        '',
    ]

    def test_same_as_circuitpython(self):
        for text in self.SAMPLES:
            want = CONSOLE.read_settings(text)
            got = {}
            for row in run(['settings'], text):
                if row[0] == 'kv':
                    got[row[1]] = unescape(row[2] if len(row) > 2 else '')
            self.assertEqual(got, want, '入力: %r' % text)


class WifiStoreTest(unittest.TestCase):
    """登録簿の読み書きが CircuitPython 版と同じか。"""

    def registry(self, nets):
        return STORE.dumps(nets)

    def test_parse_matches(self):
        cases = [
            [],
            [STORE.entry('home', '12345678', 10)],
            [STORE.entry('home', '', None), STORE.entry('cafe', 'password1', 6)],
        ]
        for nets in cases:
            text = self.registry(nets)
            want, _note = STORE.parse(text)
            got = [row[1:] for row in run(['wifiparse'], text) if row[0] == 'net']
            self.assertEqual(
                got, [[n['ssid'], n['password'], str(n['channel'] or 0)]
                      for n in want])

    def test_broken_input_is_empty_not_a_crash(self):
        for text in ['', '{', 'null', '{"networks":3}', '[]', '{"v":1}']:
            want, note = STORE.parse(text)
            rows = dict((r[0], r[1:]) for r in run(['wifiparse'], text))
            self.assertEqual(int(rows['count'][0]), len(want), text)
            # note は 'corrupt' か空。C 版も同じ判定にしてある。
            self.assertEqual(rows['note'][0] or None, note, text)

    def test_bad_entries_are_dropped(self):
        text = json.dumps({'v': 1, 'networks': [
            {'ssid': '', 'password': '12345678'},            # SSID が空
            {'ssid': 'x' * 33, 'password': '12345678'},      # 32 バイト超
            {'ssid': 'short', 'password': '1234567'},        # 8 文字未満
            {'ssid': 'chan', 'password': '', 'channel': 15},  # 14 超
            {'ssid': 'chan2', 'password': '', 'channel': True},  # bool
            {'ssid': 'ok', 'password': '12345678', 'channel': 3},
            {'ssid': 'ok', 'password': 'abcdefgh', 'channel': 4},  # 二重
        ]})
        want, _ = STORE.parse(text)
        got = [row[1:] for row in run(['wifiparse'], text) if row[0] == 'net']
        self.assertEqual([g[0] for g in got], [n['ssid'] for n in want])
        # bool の channel は None (= 0) として扱う、が現行の決まり。
        # channel が bool のものは「不明 (null)」として残る (現行と同じ)。
        self.assertEqual(got, [['chan2', '', '0'], ['ok', '12345678', '3']])

    def test_dumps_refuses_to_write_a_half_finished_registry(self):
        """入れ物に収まらないときは **0 を返して空にする**。

        半端な JSON を保存すると登録簿を丸ごと失う。以前は snprintf の
        戻り値 (「入れたかった長さ」) を足し込んでいて、入れ物の外へ
        書き出していた (ASan で再現)。
        """
        nets = [STORE.entry('a%d' % i, '1' * 63, i + 1) for i in range(8)]
        text = STORE.dumps(nets)
        rows = dict((r[0], r[1:]) for r in run(['wifidumps', '64'], text))
        self.assertEqual(rows['json'][0] if rows['json'] else '', '')
        self.assertEqual(rows.get('len', ['?'])[0], '0')

    def test_control_characters_are_refused(self):
        """SSID とパスワードに制御文字を入れさせない。

        1 文字が `\\uXXXX` の 6 バイトに膨らむので、8 件ぶんで登録簿が
        入れ物からあふれる。現行 CircuitPython 版は長さしか見ていないが、
        こちらは弾く (**登録できる中身が減るだけで、何も失わない**)。
        """
        text = json.dumps({'v': 1, 'networks': [
            {'ssid': 'a\u0001b', 'password': '12345678'},
            {'ssid': 'ok', 'password': '12345\u0017abc'},
            {'ssid': 'fine', 'password': '12345678'},
        ]})
        got = [row[1:] for row in run(['wifiparse'], text) if row[0] == 'net']
        self.assertEqual([g[0] for g in got], ['fine'])

    def test_dumps_round_trips(self):
        nets = [STORE.entry('home', '12345678', 10),
                STORE.entry('cafe', '', None)]
        text = STORE.dumps(nets)
        rows = dict((r[0], r[1:]) for r in run(['wifidumps'], text))
        again, note = STORE.parse(rows['json'][0])
        self.assertIsNone(note)
        self.assertEqual(again, nets)

    def test_upsert_keeps_position_and_limit(self):
        nets = [STORE.entry('a%d' % i, '12345678', i + 1) for i in range(8)]
        text = STORE.dumps(nets)
        # 既存の差し替えは位置が動かない
        rows = run(['wifiedit', 'add', 'a3', 'newpassword', '9'], text)
        got = [r[1:] for r in rows if r[0] == 'net']
        want, err = STORE.upsert(nets, 'a3', 'newpassword', 9)
        self.assertIsNone(err)
        self.assertEqual([g[0] for g in got], [n['ssid'] for n in want])
        self.assertEqual(got[3], ['a3', 'newpassword', '9'])
        # 9 件目は 'full'
        rows = dict((r[0], r[1:]) for r in
                    run(['wifiedit', 'add', 'zz', '12345678', '1'], text))
        self.assertEqual(rows['err'][0], 'full')
        _, err = STORE.upsert(nets, 'zz', '12345678', 1)
        self.assertEqual(err, 'full')

    def test_remove(self):
        nets = [STORE.entry('a', '12345678', 1), STORE.entry('b', '12345678', 6)]
        text = STORE.dumps(nets)
        rows = dict((r[0], r[1:]) for r in run(['wifiedit', 'del', 'a'], text))
        self.assertEqual(rows['err'][0], '')
        self.assertEqual(int(rows['count'][0]), 1)
        rows = dict((r[0], r[1:]) for r in run(['wifiedit', 'del', 'zz'], text))
        self.assertEqual(rows['err'][0], 'not_found')
        _, err = STORE.remove(nets, 'zz')
        self.assertEqual(err, 'not_found')

    def test_pick_matches_circuitpython(self):
        nets = [STORE.entry('home', 'homepass1', 10),
                STORE.entry('cafe', 'cafepass1', None)]
        text = STORE.dumps(nets)
        cases = [
            [('home', 10, -70), ('cafe', 1, -40)],
            [('home', 10, -40), ('cafe', 1, -40)],      # 同点は登録順
            [('cafe', 11, -40), ('home', 10, -40)],
            [('other', 3, -20)],                        # 登録外だけ
            [],
        ]
        for scanned in cases:
            args = []
            for ssid, ch, rssi in scanned:
                args += [ssid, str(ch), str(rssi)]
            rows = dict((r[0], r[1:]) for r in run(['wifipick'] + args, text))
            want = STORE.pick([{'ssid': s, 'ch': c, 'rssi': r}
                               for s, c, r in scanned], nets)
            if want is None:
                self.assertEqual(rows['pick'][0], '-', scanned)
                continue
            got = rows['pick']
            self.assertEqual(got[0], want['ssid'], scanned)
            self.assertEqual(got[1], want['password'], scanned)
            self.assertEqual(int(got[2]), want['channel'] or 0, scanned)
            self.assertEqual(int(got[3]), want['rssi'], scanned)
            self.assertEqual(int(got[4]), want['index'], scanned)

    def test_wifi_list_never_carries_a_password(self):
        """console の `wifi.list` が出す形そのものを確かめる。

        ★ ここが段階 3 でいちばん漏らしてはいけないところ。Web 操作盤は
        `has_password` の真偽しか受け取らない前提で作られている
        (`stackee_wifi_store.public()`)。実機の `wifi.list` は esp_wifi を
        要るので Mac では動かせないが、**外へ出す形を決めているのは
        `stackee_wifi_public_json()` 1 か所**なので、そこを直接叩く。
        """
        nets = [STORE.entry('home', 'SUPERSECRET1', 10),
                STORE.entry('open-ap', '', None),
                STORE.entry('quote"ssid', 'ANOTHERSECRET', 6)]
        text = STORE.dumps(nets)
        rows = dict((r[0], r[1:]) for r in run(['wifipublic'], text))
        body = rows['json'][0]
        # 1. そのままの文字列として現れない
        self.assertNotIn('SUPERSECRET1', body)
        self.assertNotIn('ANOTHERSECRET', body)
        # 2. `"password"` という鍵が無い (`"has_password"` とは別物)
        self.assertNotIn('"password"', body)
        # 3. 中身は CircuitPython 版の public() と同じ
        got = json.loads(body)
        want = STORE.public(nets)
        self.assertEqual(got, want)

    def test_wifi_list_refuses_to_emit_a_half_finished_array(self):
        nets = [STORE.entry('a%d' % i, '1' * 63, i + 1) for i in range(8)]
        text = STORE.dumps(nets)
        rows = dict((r[0], r[1:]) for r in run(['wifipublic', '40'], text))
        self.assertEqual(rows['len'][0], '0')
        self.assertEqual(rows['json'][0] if rows['json'] else '', '')


class ChannelOrderTest(unittest.TestCase):
    """走査するチャネルの順番と滞在時間が現行と同じか。"""

    # firmware/kmk/stackee_wifi.py の channel_order / settle_ms を写したもの。
    PATTERN = (6, 1, 11, 3, 9, 13, 2, 4, 8, 12, 5, 7, 10, 14)

    def want(self, nets, last):
        out = []
        for net in nets:
            ch = net.get('channel')
            if isinstance(ch, bool) or not isinstance(ch, int):
                continue
            if ch not in out:
                out.append(ch)
        if isinstance(last, int) and last and last not in out:
            out.append(last)
        for ch in self.PATTERN:
            if ch not in out:
                out.append(ch)
        return out

    def test_order(self):
        cases = [
            ([], 0),
            ([STORE.entry('a', '12345678', 10)], 0),
            ([STORE.entry('a', '12345678', 10)], 4),
            ([STORE.entry('a', '12345678', 10),
              STORE.entry('b', '12345678', 13)], 6),
        ]
        for nets, last in cases:
            text = STORE.dumps(nets)
            rows = dict((r[0], r[1:]) for r in run(['chorder', str(last)], text)
                        if r[0] == 'chs')
            got = [int(x) for x in rows['chs']]
            self.assertEqual(got, self.want(nets, last), (nets, last))

    def test_settle(self):
        rows = run(['chorder', '0'], STORE.dumps([]))
        for row in rows:
            if row[0] != 'settle':
                continue
            ch, ms = int(row[1]), int(row[2])
            self.assertEqual(ms, 800 if ch >= 12 else 300, ch)


class VolumeTest(unittest.TestCase):
    def test_register_bits_match_circuitpython(self):
        for row in run(['volbits']):
            if row[0] != 'bits':
                continue
            percent, bits = int(row[1]), int(row[2])
            self.assertEqual(bits, VOLUME_BITS(percent), percent)

    def test_zero_is_full_attenuation(self):
        rows = dict((int(r[1]), int(r[2])) for r in run(['volbits']))
        self.assertEqual(rows[100], 0x0000)     # 減衰なし
        self.assertGreater(rows[0], rows[50])   # 小さいほど減衰が大きい
        self.assertGreater(rows[50], rows[100])

    def test_step_and_clamp(self):
        # 20 から +5 を 20 回。100 を超えない。
        args = ['volume', '20']
        now = 0
        for _ in range(20):
            now += 10
            args += [str(now), '5', '0', '1']
        rows = [r for r in run(args) if r[0] == 'step']
        self.assertEqual(int(rows[-1][2]), 100)
        args = ['volume', '5']
        now = 0
        for _ in range(5):
            now += 10
            args += [str(now), '-5', '0', '1']
        rows = [r for r in run(args) if r[0] == 'step']
        self.assertEqual(int(rows[-1][2]), 0)

    def test_saves_only_after_two_quiet_seconds(self):
        # t=10 で +5。以後は打鍵なし・無音。2000 ms 経つまで保存しない。
        args = ['volume', '20', '10', '5', '0', '1',
                '1000', '0', '0', '0',
                '2009', '0', '0', '0',
                '2011', '0', '0', '0',
                '3000', '0', '0', '0']
        rows = [r for r in run(args) if r[0] == 'step']
        saved_at = [int(r[1]) for r in rows if r[3] == '1']
        self.assertEqual(saved_at, [2011], rows)

    def test_typing_and_audio_push_the_save_back(self):
        args = ['volume', '20', '10', '5', '0', '1',
                '2100', '0', '0', '1',          # 打鍵があった
                '2200', '0', '0', '0',
                '4099', '0', '0', '0',
                '4101', '0', '1', '0',          # 鳴っている
                '4200', '0', '0', '0']
        rows = [r for r in run(args) if r[0] == 'step']
        saved_at = [int(r[1]) for r in rows if r[3] == '1']
        self.assertEqual(saved_at, [4200], rows)


def json_rows(rows):
    """cfg json の出力を {key: value or None} にする ("none" は None)。"""
    out = {}
    for row in rows:
        out[row[1]] = None if row[0] == 'none' else unescape(row[2])
    return out


class JsonTest(unittest.TestCase):
    """サーバの応答を読む。最上位のキーだけを見ること。"""

    def test_reads_top_level_only(self):
        body = json.dumps({
            'state': 'done',
            'reply': 'ここに "state": "processing" と書いてあっても惑わされない',
            'timings': {'state': 'inner', 'sample_rate': 8000},
            'audio_url': '/jobs/abc/audio',
            'sample_rate': 16000, 'channels': 1, 'sample_width': 2,
        }, ensure_ascii=False)
        rows = json_rows(run(['json', 'state', 'reply', 'audio_url',
                              'sample_rate', 'channels', 'sample_width',
                              'missing'], body))
        self.assertEqual(rows['state'], 'done')
        self.assertIn('惑わされない', rows['reply'])
        self.assertEqual(rows['audio_url'], '/jobs/abc/audio')
        self.assertEqual(rows['sample_rate'], '16000')
        self.assertEqual(rows['channels'], '1')
        self.assertEqual(rows['sample_width'], '2')
        self.assertIsNone(rows['missing'])

    def test_escapes(self):
        body = json.dumps({'reply': 'あ\n"x"\\y', 'n': 5}, ensure_ascii=True)
        rows = json_rows(run(['json', 'reply', 'n'], body))
        self.assertEqual(rows['reply'], 'あ\n"x"\\y')
        self.assertEqual(rows['n'], '5')

    def test_truncation_lands_on_a_character_boundary(self):
        """入れ物に入り切らない返答文を、UTF-8 の途中で切らない。

        返答文の入れ物は 256 バイト (`STACKEE_TALK_TEXT_MAX`)。日本語は
        1 文字 3 バイトなので、素直に切ると末尾に「文字の途中」が残り、
        画面にも `talk.status` の JSON にも壊れたバイトが出る。
        """
        body = json.dumps({'reply': 'あいうえお'}, ensure_ascii=False)
        # cap=11 → 中身に使えるのは 10 バイト。3 文字 (9 バイト) までで、
        # 4 文字目の 1 バイト目は捨てる。
        rows = json_rows(run(['jsoncut', '11', 'reply'], body))
        self.assertEqual(rows['reply'], 'あいう')
        # ちょうど 3 文字ぶんなら 1 バイトも落とさない。
        rows = json_rows(run(['jsoncut', '10', 'reply'], body))
        self.assertEqual(rows['reply'], 'あいう')

    def test_truncation_of_escaped_text_also_lands_on_a_boundary(self):
        r"""\uXXXX で来ても同じ (もともと put_utf8 が途中書きを断る)。"""
        body = json.dumps({'reply': 'あいうえお'}, ensure_ascii=True)
        rows = json_rows(run(['jsoncut', '11', 'reply'], body))
        self.assertEqual(rows['reply'], 'あいう')

    def test_ascii_is_not_shortened_by_the_boundary_check(self):
        body = json.dumps({'reply': 'abcdefghij'})
        rows = json_rows(run(['jsoncut', '6', 'reply'], body))
        self.assertEqual(rows['reply'], 'abcde')

    def test_accept_response(self):
        body = json.dumps({'id': 'a' * 32, 'status_url': '/jobs/' + 'a' * 32})
        rows = json_rows(run(['json', 'status_url'], body))
        self.assertEqual(rows['status_url'], '/jobs/' + 'a' * 32)


class AckAssetsTest(unittest.TestCase):
    """一次回答 (pcmz) を、ファームが期待する形で読めるか。

    `stackee_audio.c` は manifest の `samples` から展開後の**丁度のバイト数**を
    計算して `stackee_assets_inflate()` に渡す。1 バイトでも違うと展開が失敗し、
    一次回答が丸ごと鳴らなくなる。ここでその前提を素材そのもので確かめる。
    実機と同じ展開器 (ROM の tinfl) ではないが、zlib 形式である以上、
    **長さと中身は一致していなければならない**。
    """

    @classmethod
    def setUpClass(cls):
        import zlib
        cls.zlib = zlib
        with open(os.path.join(KMK, 'stackee_assets/manifest.json')) as f:
            cls.manifest = json.load(f)

    def test_each_ack_matches_its_declared_length_and_hash(self):
        import hashlib
        acks = self.manifest['acks']
        self.assertEqual(len(acks), 5)
        for entry in acks:
            path = os.path.join(KMK, 'stackee_assets', entry['file'])
            raw = self.zlib.decompress(open(path, 'rb').read())
            self.assertEqual(len(raw), entry['samples'] * 2, entry['file'])
            self.assertEqual(hashlib.sha256(raw).hexdigest(),
                             entry['pcm_sha256'], entry['file'])
            # ファームは 80,000 サンプル (5 秒) を上限にしている。
            self.assertLessEqual(entry['samples'], 80000, entry['file'])
            # talk.inject は先頭 1 秒を使う。0.3 秒未満だとサーバが弾く。
            self.assertGreaterEqual(entry['samples'], 16000, entry['file'])

    def test_peak_is_the_quarter_scale_the_speaker_expects(self):
        # stackee_media.py の「ピーク 8191」。音量はレジスタで決めるので、
        # 波形側は 1/4 スケールのまま鳴らす。
        for entry in self.manifest['acks']:
            self.assertEqual(entry['peak'], 8191, entry['file'])


class UrlTest(unittest.TestCase):
    """STACKEE_TALK_URL の割り方が stackee_talk.py の parse_url と同じか。"""

    GOOD = [
        ('https://pi400.example.ts.net:8443/talk',
         'https://pi400.example.ts.net:8443', '/talk'),
        ('http://192.168.1.10:8766/talk', 'http://192.168.1.10:8766', '/talk'),
        ('https://example.com/a/b', 'https://example.com', '/a/b'),
        ('https://example.com', 'https://example.com', '/'),
    ]
    BAD = ['ftp://x/talk', 'https://', 'https://:8443/talk',
           'https://host:99999/talk', 'https://host:0/talk',
           'https://ho st/talk', 'https://user@host/talk',
           'https://host/talk#x', 'talk']

    def test_good(self):
        for url, base, path in self.GOOD:
            rows = dict((r[0], r[1] if len(r) > 1 else None)
                        for r in run(['url', url]))
            self.assertEqual(rows.get('base'), base, url)
            self.assertEqual(rows.get('path'), path, url)

    def test_bad(self):
        for url in self.BAD:
            rows = [r[0] for r in run(['url', url])]
            self.assertEqual(rows, ['bad'], url)


# ---------------------------------------------------------------------------
# 段階 4: settings.raw / settings.set の中身
# ---------------------------------------------------------------------------
# ★ 期待値は手で書かない。現行 stackee_console.py の mask_settings /
#   rewrite_settings を **そのまま呼んで**突き合わせる。


def unescape_field(text):
    """cfg_main.c の print_escaped を戻す。"""
    out = []
    i = 0
    while i < len(text):
        if text[i] == '\\' and i + 1 < len(text):
            nxt = text[i + 1]
            out.append({'n': '\n', 't': '\t', 'r': '\r',
                        '\\': '\\'}.get(nxt, nxt))
            i += 2
            continue
        out.append(text[i])
        i += 1
    return ''.join(out)


def run_text(args, text):
    rows = dict((r[0], r[1] if len(r) > 1 else '') for r in run(args, text))
    return unescape_field(rows.get('text', '')), int(rows.get('need', '0'))


SAMPLE_SETTINGS = (
    '# stackee\n'
    'STACKEE_HOST = "192.168.0.5"\n'
    'STACKEE_PORT = "8765"\n'
    'STACKEE_WIFI_PASSWORD = "placeholder-pw"\n'
    '#CIRCUITPY_WIFI_PASSWORD = "placeholder-old-pw"\n'
    'CIRCUITPY_WIFI_SSID = "ie"\n'
    '\n'
)


class JsonBigTest(unittest.TestCase):
    r"""4 KB 級の文字列を切らずに読む (done の JSON に混ざる字幕の本文)。

    字幕は `GET /jobs/<id>` の done 応答に
    `"subtitles":"<start_ms>\t<text>\n..."` として混ざってくる (README §23-1)。
    別 GET は実機で約 8 秒かかるので、本文を JSON に載せてもらう契約にした。

    ★ 返答文の 256 バイトの道 (`stackee_json_str` + `STACKEE_TALK_TEXT_MAX`)
      とは**別の道**。こちらは `stackee_json_raw` で値の範囲を取り、丸ごと
      逃がしを戻す。ここで取り違えると、字幕が 256 バイトで切れる。
    """

    def big_rows(self, body, *keys):
        out = {}
        for row in run(['jsonbig'] + list(keys), body):
            if row[0] == 'none':
                out[row[1]] = None
            elif row[0] == 'big':
                out[row[1]] = (int(row[2]), int(row[3]), unescape_field(row[4]))
            elif row[0] == 'raw':
                out[row[1] + ':raw'] = int(row[2])
        return out

    @staticmethod
    def pages(count=48, width=15):
        return ''.join('%d\t%s\n' % (i * 1000, 'あ' * width)
                       for i in range(count))

    def test_a_four_kilobyte_body_survives_intact(self):
        text = self.pages()
        self.assertGreater(len(text.encode()), 2000)
        self.assertLessEqual(len(text.encode()), 4096)
        body = json.dumps({'state': 'done', 'subtitles': text,
                           'reply': 'ほげ'}, ensure_ascii=False)
        rows = self.big_rows(body, 'subtitles', 'reply')
        need, length, got = rows['subtitles']
        self.assertEqual(got, text)
        self.assertEqual(need, len(text.encode()))
        self.assertEqual(length, len(text.encode()))
        # 生の範囲は「引用符 2 つ + 本文 + 逃がしで 1 文字ずつ増えたぶん」。
        self.assertEqual(rows['subtitles:raw'],
                         2 + len(text.encode())
                         + text.count('\t') + text.count('\n'))

    def test_tabs_and_newlines_round_trip(self):
        text = '0\tこんにちは\n1200\tさようなら\n'
        body = json.dumps({'subtitles': text}, ensure_ascii=False)
        self.assertIn(r'\t', body)          # JSON の中では逃がされている
        self.assertIn(r'\n', body)
        self.assertNotIn('\t', body)        # 生のタブは入っていない
        self.assertEqual(self.big_rows(body, 'subtitles')['subtitles'][2], text)

    def test_the_other_escapes_round_trip(self):
        # \b \f は使わない (道具の行分割が化ける。契約の本文にも出てこない)。
        text = '0\t"かぎ" \\ と / と \r\n'
        body = json.dumps({'subtitles': text}, ensure_ascii=False)
        self.assertEqual(self.big_rows(body, 'subtitles')['subtitles'][2], text)

    def test_ensure_ascii_also_works(self):
        """サーバが ensure_ascii=True に変えても読めること (\\uXXXX)。"""
        text = self.pages(count=20)
        body = json.dumps({'subtitles': text}, ensure_ascii=True)
        self.assertIn('\\u3042', body)
        rows = self.big_rows(body, 'subtitles')
        self.assertEqual(rows['subtitles'][2], text)
        # 逃がしで 2 倍近くまで膨らむ。本体の受け皿 (8192 + 2 x 4096) は
        # この膨らみを見込んである。
        self.assertGreater(rows['subtitles:raw'], 2 * len(text.encode()) * 0.9)

    def test_the_two_hundred_fifty_six_byte_path_is_a_different_one(self):
        """返答文の道と混ざっていないこと。

        `stackee_json_str` は入れ物に合わせて切る (返答文は 256 バイト)。
        字幕の本文はそこを通らない。
        """
        text = self.pages()
        body = json.dumps({'subtitles': text}, ensure_ascii=False)
        # 256 バイトの道に通すと当然切れる。
        cut = json_rows(run(['jsoncut', '256', 'subtitles'], body))
        self.assertLess(len(cut['subtitles'].encode()), 256)
        self.assertTrue(text.startswith(cut['subtitles']))
        # 4 KB の道なら 1 バイトも落ちない。
        self.assertEqual(self.big_rows(body, 'subtitles')['subtitles'][2], text)

    def test_a_missing_or_non_string_value_is_reported(self):
        body = json.dumps({'subtitles': 123, 'other': None})
        rows = run(['jsonbig', 'subtitles', 'nothere'], body)
        # 値はあるが文字列ではない → raw は返るが引用符で始まらない。
        self.assertEqual(rows[0][0], 'raw')
        self.assertEqual([r[0] for r in rows if r[1] == 'nothere'], ['none'])

    def test_a_body_with_a_quote_inside_does_not_end_early(self):
        text = '0\t"引用" の中に "subtitles":"にせもの" と書いてある\n'
        body = json.dumps({'subtitles': text, 'state': 'done'},
                          ensure_ascii=False)
        rows = self.big_rows(body, 'subtitles')
        self.assertEqual(rows['subtitles'][2], text)
        self.assertEqual(json_rows(run(['json', 'state'], body))['state'], 'done')


class MaskTest(unittest.TestCase):
    """パスワードの値が 1 文字も出ないこと。"""

    CASES = [
        SAMPLE_SETTINGS,
        '',
        'STACKEE_HOST = "a"',
        'CIRCUITPY_WEB_API_PASSWORD="x"\n',
        '   # CIRCUITPY_WIFI_PASSWORD = "placeholder"\n',
        'STACKEE_PORT = 8080\nSTACKEE_WIFI_PASSWORD=placeholder-pw\n',
    ]

    def test_matches_circuitpython(self):
        for text in self.CASES:
            got, _need = run_text(['mask'], text)
            want = CONSOLE.mask_settings(text)
            self.assertEqual(want, got, repr(text))

    def test_no_secret_leaks(self):
        got, _need = run_text(['mask'], SAMPLE_SETTINGS)
        self.assertNotIn('placeholder-pw', got)
        self.assertNotIn('placeholder-old-pw', got)
        self.assertIn('"***"', got)


class RewriteTest(unittest.TestCase):

    def check(self, text, kv):
        args = ['rewrite']
        for key in kv:
            args += [key, '--null' if kv[key] is None else kv[key]]
        got, _need = run_text(args, text)
        want = CONSOLE.rewrite_settings(text, kv)
        self.assertEqual(want, got, '%r %r' % (text, kv))

    def test_replace_existing(self):
        self.check(SAMPLE_SETTINGS, {'STACKEE_HOST': '10.0.0.9'})

    def test_append_missing(self):
        self.check(SAMPLE_SETTINGS, {'STACKEE_TALK_URL': 'https://x/talk'})

    def test_delete(self):
        self.check(SAMPLE_SETTINGS, {'STACKEE_PORT': None})

    def test_two_keys(self):
        self.check(SAMPLE_SETTINGS,
                   {'STACKEE_HOST': '1.2.3.4', 'STACKEE_PORT': '99'})

    def test_empty_file(self):
        self.check('', {'STACKEE_HOST': 'a'})

    def test_no_trailing_newline(self):
        self.check('STACKEE_HOST = "a"', {'STACKEE_PORT': 'b'})

    def test_comment_lines_are_untouched(self):
        text = '#STACKEE_HOST = "old"\nSTACKEE_PORT = "1"\n'
        self.check(text, {'STACKEE_HOST': 'new'})
        got, _need = run_text(['rewrite', 'STACKEE_HOST', 'new'], text)
        self.assertIn('#STACKEE_HOST = "old"', got)

    def test_escapes_quotes(self):
        self.check(SAMPLE_SETTINGS, {'STACKEE_HOST': 'a"b\\c'})


class AllowedKeysTest(unittest.TestCase):
    """書き換えてよいキー / 値を出してはいけないキーの表。"""

    def test_allowed(self):
        rows = dict((r[1], (r[2], r[3])) for r in run(
            ['allowed', 'STACKEE_HOST', 'STACKEE_PORT', 'STACKEE_TALK_URL',
             'STACKEE_TALK_TOKEN', 'CIRCUITPY_WIFI_SSID',
             'STACKEE_WIFI_PASSWORD', 'CIRCUITPY_WEB_API_PASSWORD']))
        self.assertEqual(('1', '0'), rows['STACKEE_HOST'])
        self.assertEqual(('1', '0'), rows['STACKEE_PORT'])
        self.assertEqual(('1', '0'), rows['STACKEE_TALK_URL'])
        # トークンは書けるが、値は返さない。
        self.assertEqual(('1', '1'), rows['STACKEE_TALK_TOKEN'])
        # ★ CIRCUITPY_WIFI_SSID は書かせない (起動が 19 秒延びる)。
        self.assertEqual(('0', '0'), rows['CIRCUITPY_WIFI_SSID'])
        self.assertEqual(('0', '1'), rows['STACKEE_WIFI_PASSWORD'])
        self.assertEqual(('0', '1'), rows['CIRCUITPY_WEB_API_PASSWORD'])

    def test_secret_list_matches_circuitpython(self):
        # 現行が伏せているキーは、C 版でも必ず伏せる (増えるのは構わない)。
        rows = dict((r[1], r[3]) for r in run(
            ['allowed'] + list(CONSOLE.SECRET_KEYS)))
        for key in CONSOLE.SECRET_KEYS:
            self.assertEqual('1', rows[key], key)


if __name__ == '__main__':
    unittest.main(verbosity=2)
