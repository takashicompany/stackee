#!/usr/bin/env python3
"""段階 4: Raw HID の上のコンソールを Mac 上で突き合わせる。**実機には触らない。**

同じ枠を 3 か所が別々に実装している。**3 つとも同じ形を作らないと壊れる。**

  1. デバイス側   firmware/main/stackee_conhid.c       (ここでは hostbuild 経由)
  2. Mac 側       firmware/tools/console_hid.py
  3. ブラウザ側   docs/js/hid.js                        (node があれば一緒に見る)

見るのは 4 つ:
  ・要求を 29 バイトずつに割って送り、デバイスが 1 バイトも落とさず組み直せるか
  ・デバイスが返すバイト列が、CDC のときと**まったく同じ**か
    (= 既存の FrameParser でそのまま切り分けられるか)
  ・溢れたときに数えられるか (黙って壊れないか)
  ・**ホストが読んでいないときにログを溜めないか**、そのとき投げた応答が
    ちゃんと入るか (実機で踏んだ。溜めると環状バッファが起動ログで埋まり、
    hello の応答が捨てられてコンソールが永久に黙る)
  ・VIA の本来のコマンド (0x01 など) は素通しされるか

  python3 firmware/tools/test_conhid_host.py
"""
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
IDF = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import stackee_tree as tree       # noqa: E402

WEB = str(tree.WEB) if tree.WEB else os.path.join(IDF, '_no_docs_js')

import console_hid as ch          # noqa: E402

if not tree.add_kmk_tools(sys.path):
    raise unittest.SkipTest(
        '現行 CircuitPython 版の道具 (firmware/kmk/tools) が無いので、'
        'Mac 側の枠との突き合わせはできない')
import stackee_console_client as ccl    # noqa: E402

SANITIZE = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
_BINARY = None


def binary():
    global _BINARY
    if _BINARY is None:
        out = os.path.join(tempfile.mkdtemp(), 'conhid')
        cmd = (['cc', '-O1', '-std=gnu11', '-Wall', '-Werror'] + SANITIZE + [
            '-I', os.path.join(IDF, 'main'), '-o', out,
            os.path.join(IDF, 'hostbuild/conhid_main.c'),
            os.path.join(IDF, 'main/stackee_conhid.c')])
        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError('ホストビルドに失敗:\n' + build.stderr)
        _BINARY = out
    return _BINARY


def run(script):
    out = subprocess.run([binary()], input=script, capture_output=True,
                         text=True)
    if out.returncode != 0:
        raise AssertionError('実行に失敗:\n' + out.stderr)
    return [line.split('\t') for line in out.stdout.splitlines()]


def hexs(data):
    return data.hex()


class FramingTest(unittest.TestCase):
    """Mac 側が作る形を、デバイス側がそのまま読めるか。"""

    def test_one_request_round_trip(self):
        line = ccl.encode_request(1, 'status')
        script = ''
        for rep in ch.encode_tx_reports(line):
            script += 'tx %s\n' % hexs(rep)
        script += 'read\n'
        rows = run(script)
        reads = [r for r in rows if r[0] == 'read']
        self.assertEqual(1, len(reads))
        self.assertEqual(line, bytes.fromhex(reads[0][1]))

    def test_long_request_is_split(self):
        # 29 バイトを超える要求 (Wi-Fi の追加など) が何枚にも割れて届く。
        line = ccl.encode_request(7, 'wifi.add', ssid='a' * 30,
                                  password='b' * 40, channel=6)
        reports = ch.encode_tx_reports(line)
        self.assertGreater(len(reports), 2)
        for rep in reports:
            self.assertEqual(32, len(rep))
        script = ''.join('tx %s\n' % hexs(r) for r in reports) + 'read\n'
        rows = run(script)
        reads = [r for r in rows if r[0] == 'read']
        self.assertEqual(line, bytes.fromhex(reads[0][1]))

    def test_ack_counts_the_bytes(self):
        line = ccl.encode_request(1, 'hello')
        rep = ch.encode_tx_reports(line)[0]
        rows = run('tx %s\n' % hexs(rep))
        reply = bytes.fromhex([r for r in rows if r[0] == 'reply'][0][1])
        # ★ 0xC0 の応答だけ byte1 の意味が違う (「受け取ったバイト数」)。
        #   payload は使わない。
        self.assertEqual(ch.CMD_TX, reply[0])
        self.assertEqual(rep[1], reply[1])
        self.assertEqual(0, reply[2], '入り切らなかったと言っている')

    def test_empty_request_is_harmless(self):
        rows = run('tx c000000000\n' + 'read\n')
        reads = [r for r in rows if r[0] == 'read']
        self.assertEqual('', reads[0][1] if len(reads[0]) > 1 else '')


class ReceiveTest(unittest.TestCase):
    """デバイス → ホストが、CDC と同じバイト列になるか。"""

    def drain(self, script_prefix):
        """0xC1 を続けざまに撃って、返ってきた payload を全部つなぐ。"""
        script = script_prefix + 'rx\n' * 400
        rows = run(script)
        out = bytearray()
        for row in rows:
            if row[0] != 'reply':
                continue
            rep = bytes.fromhex(row[1])
            rid, payload, more = ch.decode_rx(rep)
            self.assertEqual(ch.CMD_RX, rid)
            out += payload
            if not payload and not more:
                break
        return bytes(out)

    def test_frame_and_log_are_interleaved_as_on_cdc(self):
        # console が出すのと同じ順: ログ → 応答枠 → ログ
        body = (b'I (123) stackee: hello\n'
                b'\x1e{"id":1,"fw":"stackee-idf/4"}\n'
                b'W (124) stackee: bye\n')
        got = self.drain('write %s\n' % hexs(body))
        self.assertEqual(body, got)
        # ★ 既存の切り分け器がそのまま通ること。これが通らないなら
        #   full プロファイルで操作盤が動かない。
        frames, log = ccl.FrameParser().feed(got)
        self.assertEqual([{'id': 1, 'fw': 'stackee-idf/4'}], frames)
        self.assertIn('hello', log)
        self.assertIn('bye', log)

    def test_more_flag(self):
        body = bytes(range(0, 100))
        rows = run('write %s\nrx\n' % hexs(body))
        rep = bytes.fromhex([r for r in rows if r[0] == 'reply'][0][1])
        rid, payload, more = ch.decode_rx(rep)
        self.assertEqual(29, len(payload))
        self.assertTrue(more, '29 バイトしか渡していないのに more が立たない')

    def test_empty_when_nothing_pending(self):
        rows = run('rx\n')
        rep = bytes.fromhex([r for r in rows if r[0] == 'reply'][0][1])
        rid, payload, more = ch.decode_rx(rep)
        self.assertEqual(b'', payload)
        self.assertFalse(more)

    def test_big_reply_survives(self):
        # status の応答は 1,800 バイトほど。1 枚も落とさずに渡せること。
        body = b'\x1e' + json.dumps(
            {'id': 1, 'pad': 'x' * 1700}, separators=(',', ':')
        ).encode() + b'\n'
        got = self.drain('write %s\n' % hexs(body))
        self.assertEqual(body, got)


class LogGateTest(unittest.TestCase):
    """★ ホストが読んでいないときにログを溜めない（実機で踏んだ欠陥）。"""

    def test_log_is_dropped_until_the_host_polls(self):
        # 0xC1 を 1 度も撃っていない = 誰も読んでいない。ログは捨てる。
        rows = run('tick 1000\nlog %s\nrx\n' % hexs(b'L' * 500))
        rep = bytes.fromhex([r for r in rows if r[0] == 'reply'][0][1])
        _rid, payload, more = ch.decode_rx(rep)
        self.assertEqual(b'', payload, 'ホストが読む前からログを溜めている')
        self.assertFalse(more)

    def test_log_is_kept_after_a_poll(self):
        rows = run('tick 1000\nrx\nlog %s\nrx\n' % hexs(b'L' * 40))
        reps = [bytes.fromhex(r[1]) for r in rows if r[0] == 'reply']
        _rid, payload, _more = ch.decode_rx(reps[1])
        self.assertEqual(b'L' * 29, payload)

    def test_log_stops_again_after_the_host_goes_away(self):
        # 0xC1 のあと 4 秒 (窓は 3 秒) たったら、また捨てる。
        rows = run('tick 1000\nrx\ntick 5000\nlog %s\nrx\n' % hexs(b'L' * 40))
        reps = [bytes.fromhex(r[1]) for r in rows if r[0] == 'reply']
        _rid, payload, _more = ch.decode_rx(reps[-1])
        self.assertEqual(b'', payload)

    def test_reply_gets_in_even_when_the_ring_is_full(self):
        """★ これが実機で壊れていた。

        ホストが読まないあいだに溜まった古い中身で環状バッファが満杯でも、
        **いま作った応答は必ず入る**こと（古いほうを押し出す）。
        """
        old = b'x' * 4000                     # 環状バッファ (3072) より多い
        reply = b'\x1e{"id":1,"fw":"stackee-idf/4"}\n'
        rows = run('write %s\nwrite %s\n' % (hexs(old), hexs(reply))
                   + 'rx\n' * 200)
        out = bytearray()
        for row in rows:
            if row[0] != 'reply':
                continue
            _rid, payload, more = ch.decode_rx(bytes.fromhex(row[1]))
            out += payload
            if not payload and not more:
                break
        self.assertIn(reply, bytes(out), '新しい応答が捨てられている')
        stats = [r for r in rows if r[0] == 'stats'][0]
        self.assertGreater(int(stats[3]), 0, '押し出した回数を数えていない')


class OverflowTest(unittest.TestCase):
    """溢れたら黙って壊れず、数えること。"""

    def test_log_ring_overflow_is_counted(self):
        # 環状バッファは 3072 バイト。読んでいる相手が居る状態で溢れさせる。
        rows = run('tick 1000\nrx\nlog %s\n' % hexs(b'z' * 4000))
        stats = [r for r in rows if r[0] == 'stats'][0]
        pending, dropped = int(stats[1]), int(stats[2])
        self.assertGreater(dropped, 0, '溢れたのに数えていない')
        self.assertLessEqual(pending, 3072)

    def test_rx_ring_overflow_is_reported(self):
        # 受信側は 1024 バイト。読み出さずに詰め続けると ack の len が減る。
        rep = bytearray(32)
        rep[0] = ch.CMD_TX
        rep[1] = 29
        rep[3:32] = b'q' * 29
        script = 'tx %s\n' % hexs(bytes(rep)) * 1
        script = ('tx %s\n' % hexs(bytes(rep))) * 60
        rows = run(script)
        acks = [bytes.fromhex(r[1]) for r in rows if r[0] == 'reply']
        short = [a for a in acks if a[1] != 29]
        self.assertTrue(short, '入り切らないのに全部受け取ったと言っている')
        self.assertEqual(0x01, short[0][2] & 0x01, 'flags に印が立っていない')


class PassThroughTest(unittest.TestCase):
    """VIA の本来のコマンドは触らないこと。"""

    def test_via_command_is_not_stolen(self):
        # 0x01 = VIA の get_keyboard_value。こちらは false を返して
        # via.c にそのまま渡さなければならない。
        rep = bytearray(32)
        rep[0] = 0x01
        rows = run('raw %s\n' % hexs(bytes(rep)))
        self.assertEqual(['noreply'], rows[0])

    def test_unknown_high_id_is_not_stolen(self):
        rep = bytearray(32)
        rep[0] = 0xC5       # 0xC0..0xC2 の外
        rows = run('raw %s\n' % hexs(bytes(rep)))
        self.assertEqual(['noreply'], rows[0])


class InfoTest(unittest.TestCase):

    def test_info_reports_proto_and_pending(self):
        rows = run('write %s\ninfo\n' % hexs(b'x' * 50))
        rep = bytes.fromhex([r for r in rows if r[0] == 'reply'][0][1])
        info = ch.parse_info(rep)
        self.assertEqual(ch.CONHID_PROTO, info['proto'])
        self.assertEqual(50, info['pending'])
        self.assertEqual(0, info['dropped'])


class BrowserAgreementTest(unittest.TestCase):
    """docs/js/hid.js が同じ形を作るか (node があれば)。"""

    def setUp(self):
        if not os.path.exists(os.path.join(WEB, 'hid.js')):
            self.skipTest('docs/js/hid.js が無い')
        try:
            subprocess.run(['node', '--version'], capture_output=True,
                           check=True)
        except Exception:
            self.skipTest('node が無い')

    def node_encode(self, data):
        script = (
            "import {encodeTxReports} from '%s';\n"
            "const d = Buffer.from(process.argv[2], 'hex');\n"
            "const out = encodeTxReports(new Uint8Array(d));\n"
            "console.log(out.map(r => Buffer.from(r).toString('hex')).join(' '));\n"
            % os.path.join(WEB, 'hid.js'))
        path = os.path.join(tempfile.mkdtemp(), 'enc.mjs')
        open(path, 'w').write(script)
        out = subprocess.run(['node', path, data.hex()], capture_output=True,
                             text=True)
        if out.returncode != 0:
            raise AssertionError(out.stderr)
        return [bytes.fromhex(x) for x in out.stdout.split()]

    def test_same_reports_as_python(self):
        for line in (ccl.encode_request(1, 'status'),
                     ccl.encode_request(2, 'wifi.add', ssid='a' * 40,
                                        password='b' * 60),
                     b'', b'x'):
            self.assertEqual(ch.encode_tx_reports(line),
                             self.node_encode(line),
                             '割り方がブラウザ側と違う: %r' % line)


if __name__ == '__main__':
    unittest.main(verbosity=2)
