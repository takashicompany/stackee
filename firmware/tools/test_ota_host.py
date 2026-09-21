#!/usr/bin/env python3
"""アプリ内 OTA の中核を Mac 上で突き合わせる。**実機には触らない。**

同じ枠を 2 か所が別々に実装している。**どちらも同じ形を作らないと壊れる。**

  1. デバイス側   firmware/main/stackee_otacore.c   (ここでは hostbuild 経由)
  2. ブラウザ側   docs/js/ota.js                    (node があれば一緒に見る)
     — Node 側の道具 (firmware/tools/ota.mjs) は同じ ota.js を import する

見るのは 7 つ:
  ・0xC3 の枠 (id / len / 位置 24 bit / 本文 27 バイト) の組み立てと分解
  ・環状バッファが 1 バイトも落とさず・並べ替えずに通すか
  ・credit の材料 (accepted / written / 空き) の数え方
  ・SHA-256 をストリーミングで計算した値が hashlib と一致するか
  ・断りどころ — 二重 begin / 位置違い / magic 違い / sha 違い / 長さ違い /
    溢れ / 無通信の自動 abort / フラッシュの失敗
  ・途中で切れて abort してから、最初からやり直せるか
  ・`ota.mjs --commit` を**像なしで**叩いたとき (書き終えてある next へ
    切り替えるだけ) の断りどころと歩き方

  python3 firmware/tools/test_ota_host.py
"""
import hashlib
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

SANITIZE = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
_BINARY = None

# stackee_otacore.h と同じ値。食い違ったらテストが落ちる。
CMD_DATA = 0xC3
REPORT = 32
HEADER = 5
PAYLOAD = 27
CREDIT = 32 * 1024
RING_MIN = 64 * 1024
ACK_EVERY = 1024
IDLE_MS = 30000


def binary():
    global _BINARY
    if _BINARY is None:
        out = os.path.join(tempfile.mkdtemp(), 'ota')
        cmd = (['cc', '-O1', '-std=gnu11', '-Wall', '-Werror'] + SANITIZE + [
            '-I', os.path.join(IDF, 'main'),
            '-I', os.path.join(IDF, 'hostbuild/stub'),
            '-o', out,
            os.path.join(IDF, 'hostbuild/ota_main.c'),
            os.path.join(IDF, 'main/stackee_otacore.c'),
            os.path.join(IDF, 'hostbuild/stub/sha256_stub.c')])
        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError('ホストビルドに失敗:\n' + build.stderr)
        _BINARY = out
    return _BINARY


def run(script):
    out = subprocess.run([binary()], input=script, capture_output=True, text=True)
    if out.returncode != 0:
        raise AssertionError('実行に失敗:\n' + out.stderr)
    return [line.split('\t') for line in out.stdout.splitlines()]


def image(size, first=0xE9):
    """0xE9 で始まる作り物の像。"""
    data = bytearray(size)
    data[0] = first
    for i in range(1, size):
        data[i] = (i * 31 + (i >> 8)) & 0xFF
    return bytes(data)


def send_script(data, ring=RING_MIN, bounce=4096, pump_every=64,
                sha=None, size=None):
    """像を丸ごと流す台本を作る。"""
    want = sha if sha is not None else hashlib.sha256(data).hexdigest()
    lines = ['attach %d %d' % (ring, bounce),
             'begin %d %s' % (size if size is not None else len(data), want)]
    n = 0
    for off in range(0, len(data), PAYLOAD):
        chunk = data[off:off + PAYLOAD]
        lines.append('data %d %s' % (off, chunk.hex()))
        n += 1
        if pump_every and n % pump_every == 0:
            lines.append('pump %d' % (CREDIT,))
    lines.append('pump %d' % (CREDIT,))
    lines.append('end')
    lines.append('flash')
    lines.append('status')
    return '\n'.join(lines) + '\n'


def field(rows, tag, index=1):
    for row in rows:
        if row[0] == tag:
            return row[index]
    raise AssertionError('%s の行が無い: %r' % (tag, rows))


def replies(rows):
    return [bytes.fromhex(r[1]) for r in rows if r[0] == 'reply']


def parse_reply(rep):
    assert rep[0] == CMD_DATA
    u32 = lambda i: int.from_bytes(rep[i:i + 4], 'little')   # noqa: E731
    return {'state': rep[1], 'err': rep[2], 'accepted': u32(3), 'written': u32(7),
            'size': u32(11), 'rejected': bool(rep[15] & 1), 'free': u32(16)}


# ---------------------------------------------------------------------------
class FrameTest(unittest.TestCase):
    """0xC3 の枠の形。"""

    def test_status_only_frame_answers_even_when_idle(self):
        rows = run('attach 65536 4096\npoll\n')
        rep = parse_reply(replies(rows)[0])
        self.assertEqual(rep['state'], 0)        # idle
        self.assertEqual(rep['accepted'], 0)

    def test_other_command_ids_are_not_ours(self):
        # 0xC0 (コンソールの送信) は OTA のものではない = via_command_kb の
        # 本来の処理へ回る。ここを取りこぼすとコンソールが黙る。
        rows = run('attach 65536 4096\nraw c0010000\n')
        self.assertEqual(rows[-1][0], 'notmine')

    def test_offset_is_24bit_little_endian(self):
        data = image(200)
        # 位置 27 の枠を「先に」送ると断られ、応答の accepted は 0 のまま。
        script = ('attach 65536 4096\nbegin 200 %s\n'
                  'data 27 %s\nstatus\n'
                  % (hashlib.sha256(data).hexdigest(), data[27:54].hex()))
        rows = run(script)
        rep = parse_reply(replies(rows)[0])
        self.assertTrue(rep['rejected'])
        self.assertEqual(rep['accepted'], 0)
        self.assertEqual(field(rows, 'status', 7), '1')     # rejected が 1


class HappyPathTest(unittest.TestCase):
    """素直に全部流したとき。"""

    def test_small_image_round_trip(self):
        data = image(4321)
        rows = run(send_script(data))
        self.assertEqual(field(rows, 'end', 2), 'none')
        flash = [r for r in rows if r[0] == 'flash'][0]
        self.assertEqual(int(flash[1]), len(data))
        self.assertEqual(flash[2], hashlib.sha256(data).hexdigest())
        self.assertEqual(field(rows, 'status', 1), 'done')

    def test_image_larger_than_the_ring(self):
        # 環状バッファ (64 KB) を 3 周する = 巻き戻りの継ぎ目が正しいか。
        data = image(200 * 1024)
        rows = run(send_script(data))
        self.assertEqual(field(rows, 'end', 2), 'none')
        flash = [r for r in rows if r[0] == 'flash'][0]
        self.assertEqual(int(flash[1]), len(data))
        self.assertEqual(flash[2], hashlib.sha256(data).hexdigest())

    def test_streaming_sha_matches_hashlib(self):
        data = image(70000)
        rows = run(send_script(data) + 'sha\n')
        got = [r for r in rows if r[0] == 'sha'][0]
        self.assertEqual(got[1], '1')
        self.assertEqual(got[2], hashlib.sha256(data).hexdigest())

    def test_tail_shorter_than_the_bounce_buffer(self):
        # 4096 の倍数でない長さ = 最後の端数がちゃんと書かれるか。
        data = image(4096 * 3 + 7)
        rows = run(send_script(data))
        self.assertEqual(field(rows, 'end', 2), 'none')
        self.assertEqual(int([r for r in rows if r[0] == 'flash'][0][1]), len(data))

    def test_one_byte_image(self):
        data = bytes([0xE9])
        rows = run(send_script(data))
        self.assertEqual(field(rows, 'end', 2), 'none')
        self.assertEqual([r for r in rows if r[0] == 'flash'][0][2],
                         hashlib.sha256(data).hexdigest())


class CreditTest(unittest.TestCase):
    """本体が返す数字 (credit の材料)。"""

    def test_reply_every_1k_and_at_the_end(self):
        data = image(8000)
        rows = run(send_script(data, pump_every=0))
        got = [parse_reply(r) for r in replies(rows)]
        # 1 KB ごと + 末尾。1 枚ごとには返さない (送信キューを埋めないため)。
        self.assertLess(len(got), len(data) // PAYLOAD // 4)
        self.assertGreaterEqual(len(got), len(data) // ACK_EVERY)
        for prev, cur in zip(got, got[1:-1]):
            self.assertGreater(cur['accepted'], prev['accepted'])
            self.assertGreaterEqual(cur['accepted'] - prev['accepted'], ACK_EVERY)
        # 最後の 1 枚だけは「像を受け切った」合図なので間隔が短くてよい。
        self.assertGreater(got[-1]['accepted'], got[-2]['accepted'])
        self.assertEqual(got[-1]['accepted'], len(data))

    def test_free_space_shrinks_as_the_ring_fills(self):
        data = image(4000)
        # pump しないで積むと、空きがそのぶん減る。
        rows = run(send_script(data, pump_every=0).split('pump')[0]
                   + '\npoll\n')
        rep = parse_reply(replies(rows)[-1])
        self.assertEqual(rep['free'], RING_MIN - rep['accepted'])
        self.assertEqual(rep['written'], 0)

    def test_written_follows_pump(self):
        data = image(20000)
        script = ('attach %d 4096\nbegin %d %s\n'
                  % (RING_MIN, len(data), hashlib.sha256(data).hexdigest()))
        for off in range(0, len(data), PAYLOAD):
            script += 'data %d %s\n' % (off, data[off:off + PAYLOAD].hex())
        script += 'poll\npump 4096\npoll\n'
        rows = run(script)
        reps = [parse_reply(r) for r in replies(rows)]
        self.assertEqual(reps[-2]['written'], 0)
        self.assertEqual(reps[-1]['written'], 4096)
        self.assertEqual(reps[-1]['free'], RING_MIN - (len(data) - 4096))

    def test_ring_overflow_is_counted_not_silent(self):
        # credit を守らずに環状バッファ (64 KB) より多く積むと、
        # そこから先は捨てられて数えられる (黙って壊れない)。
        data = image(70000)
        script = ('attach %d 4096\nbegin %d %s\n'
                  % (RING_MIN, len(data), hashlib.sha256(data).hexdigest()))
        for off in range(0, len(data), PAYLOAD):
            script += 'data %d %s\n' % (off, data[off:off + PAYLOAD].hex())
        script += 'status\n'
        rows = run(script)
        status = [r for r in rows if r[0] == 'status'][0]
        accepted = int(status[4])
        # 27 バイト刻みなので、ぴったり 64 KB ではなく「入るところまで」。
        self.assertGreater(accepted, RING_MIN - PAYLOAD)
        self.assertLessEqual(accepted, RING_MIN)
        self.assertGreater(int(status[8]), 0)               # overflow


class RejectTest(unittest.TestCase):
    """断りどころ。"""

    def test_double_begin_is_refused(self):
        data = image(100)
        sha = hashlib.sha256(data).hexdigest()
        rows = run('attach 65536 4096\nbegin 100 %s\nbegin 100 %s\n' % (sha, sha))
        begins = [r for r in rows if r[0] == 'begin']
        self.assertEqual(begins[0][2], 'none')
        self.assertEqual(begins[1][2], 'busy')

    def test_wrong_magic_is_refused_before_any_flash_write(self):
        data = image(100, first=0x7F)
        rows = run(send_script(data))
        self.assertEqual(field(rows, 'status', 1), 'failed')
        self.assertEqual(field(rows, 'status', 2), 'magic')
        flash = [r for r in rows if r[0] == 'flash'][0]
        self.assertEqual(int(flash[1]), 0, 'magic 違いなのに 1 バイト書いている')
        self.assertEqual(int(flash[5]), 1, 'esp_ota_abort に当たる後始末が無い')

    def test_sha_mismatch_is_refused(self):
        data = image(3000)
        rows = run(send_script(data, sha='11' * 32))
        self.assertEqual(field(rows, 'end', 2), 'sha')
        self.assertEqual(field(rows, 'status', 1), 'failed')
        # ★ 「書けたか」と「合っているか」は別。全部書いた上で断る。
        self.assertEqual(int([r for r in rows if r[0] == 'flash'][0][1]), len(data))

    def test_short_image_is_refused(self):
        data = image(3000)
        # 申告より短いところで end する。
        rows = run(send_script(data, size=4000))
        self.assertEqual(field(rows, 'end', 2), 'size')

    def test_extra_bytes_past_the_declared_size_are_dropped(self):
        data = image(100)
        script = ('attach 65536 4096\nbegin 60 %s\n'
                  % hashlib.sha256(data[:60]).hexdigest())
        for off in range(0, 100, PAYLOAD):
            script += 'data %d %s\n' % (off, data[off:off + PAYLOAD].hex())
        script += 'pump 65536\nend\nflash\n'
        rows = run(script)
        self.assertEqual(field(rows, 'end', 2), 'none')
        self.assertEqual(int([r for r in rows if r[0] == 'flash'][0][1]), 60)

    def test_data_without_begin_is_refused(self):
        rows = run('attach 65536 4096\ndata 0 e900\nstatus\n')
        rep = parse_reply(replies(rows)[0])
        self.assertTrue(rep['rejected'])
        self.assertEqual(field(rows, 'status', 2), 'state')

    def test_begin_without_buffers_is_refused(self):
        rows = run('attach 0 0\nbegin 100 %s\n' % ('00' * 32))
        self.assertEqual(field(rows, 'begin', 2), 'nomem')

    def test_ring_smaller_than_the_minimum_is_refused(self):
        rows = run('attach 4096 4096\nbegin 100 %s\n' % ('00' * 32))
        self.assertEqual(field(rows, 'begin', 2), 'nomem')

    def test_size_beyond_24bit_is_refused(self):
        rows = run('attach 65536 4096\nbegin 16777216 %s\n' % ('00' * 32))
        self.assertEqual(field(rows, 'begin', 2), 'arg')

    def test_flash_begin_failure_is_reported(self):
        rows = run('attach 65536 4096\nfail begin 1\nbegin 100 %s\nstatus\n'
                   % ('00' * 32))
        self.assertEqual(field(rows, 'begin', 2), 'flash_begin')
        self.assertEqual(field(rows, 'status', 1), 'failed')

    def test_flash_write_failure_stops_the_transfer(self):
        data = image(9000)
        script = ('attach 65536 4096\nbegin %d %s\n'
                  % (len(data), hashlib.sha256(data).hexdigest()))
        for off in range(0, len(data), PAYLOAD):
            script += 'data %d %s\n' % (off, data[off:off + PAYLOAD].hex())
        script += 'fail write 1\npump 65536\nstatus\nflash\n'
        rows = run(script)
        self.assertEqual(field(rows, 'status', 1), 'failed')
        self.assertEqual(field(rows, 'status', 2), 'flash_write')
        self.assertEqual(int([r for r in rows if r[0] == 'flash'][0][5]), 1)

    def test_flash_end_failure_is_reported(self):
        data = image(3000)
        rows = run(send_script(data).replace('\nend\n', '\nfail end 1\nend\n'))
        self.assertEqual(field(rows, 'end', 2), 'flash_end')
        self.assertEqual(field(rows, 'status', 1), 'failed')


class RecoveryTest(unittest.TestCase):
    """途中で切れたあと。"""

    def test_lost_frame_then_resend_from_accepted(self):
        data = image(2000)
        script = ('attach 65536 4096\nbegin %d %s\n'
                  % (len(data), hashlib.sha256(data).hexdigest()))
        skip = 10       # この枠だけ「届かなかった」ことにする
        for i, off in enumerate(range(0, len(data), PAYLOAD)):
            if i == skip:
                continue
            script += 'data %d %s\n' % (off, data[off:off + PAYLOAD].hex())
        script += 'poll\n'
        rows = run(script)
        rep = parse_reply(replies(rows)[-1])
        self.assertEqual(rep['accepted'], skip * PAYLOAD)
        # ホスト側の作法どおり accepted から送り直す。
        script2 = script
        for off in range(skip * PAYLOAD, len(data), PAYLOAD):
            script2 += 'data %d %s\n' % (off, data[off:off + PAYLOAD].hex())
        script2 += 'pump 65536\nend\nflash\n'
        rows2 = run(script2)
        self.assertEqual(field(rows2, 'end', 2), 'none')
        flash = [r for r in rows2 if r[0] == 'flash'][0]
        self.assertEqual(int(flash[1]), len(data))
        self.assertEqual(flash[2], hashlib.sha256(data).hexdigest())

    def test_abort_then_start_over(self):
        data = image(5000)
        sha = hashlib.sha256(data).hexdigest()
        script = 'attach 65536 4096\nbegin %d %s\n' % (len(data), sha)
        for off in range(0, 1000, PAYLOAD):
            script += 'data %d %s\n' % (off, data[off:off + PAYLOAD].hex())
        script += 'abort\nstatus\n'
        # 最初からやり直す
        script += 'begin %d %s\n' % (len(data), sha)
        for off in range(0, len(data), PAYLOAD):
            script += 'data %d %s\n' % (off, data[off:off + PAYLOAD].hex())
        script += 'pump 65536\nend\nflash\n'
        rows = run(script)
        self.assertEqual(field(rows, 'status', 1), 'idle')
        self.assertEqual(field(rows, 'end', 2), 'none')
        flash = [r for r in rows if r[0] == 'flash'][0]
        self.assertEqual(int(flash[1]), len(data))
        self.assertEqual(flash[2], sha)
        self.assertEqual(int(flash[3]), 2, 'esp_ota_begin が 2 回呼ばれていない')
        self.assertEqual(int(flash[5]), 1, 'abort で後始末していない')

    def test_idle_timeout_aborts_by_itself(self):
        data = image(2000)
        script = ('attach 65536 4096\nbegin %d %s\n'
                  % (len(data), hashlib.sha256(data).hexdigest()))
        script += 'data 0 %s\n' % data[:PAYLOAD].hex()
        script += 'tick %d\nstatus\nflash\n' % (IDLE_MS + 1)
        rows = run(script)
        self.assertEqual(field(rows, 'status', 1), 'failed')
        self.assertEqual(field(rows, 'status', 2), 'idle')
        self.assertEqual(int([r for r in rows if r[0] == 'flash'][0][5]), 1)

    def test_idle_timer_resets_on_every_frame(self):
        data = image(2000)
        script = ('attach 65536 4096\nbegin %d %s\n'
                  % (len(data), hashlib.sha256(data).hexdigest()))
        for off in range(0, len(data), PAYLOAD):
            script += 'tick %d\n' % (IDLE_MS - 1)
            script += 'data %d %s\n' % (off, data[off:off + PAYLOAD].hex())
        script += 'pump 65536\nend\n'
        rows = run(script)
        self.assertEqual(field(rows, 'end', 2), 'none')

    def test_failed_state_survives_until_the_next_begin(self):
        data = image(200, first=0x00)
        rows = run(send_script(data) + 'poll\n')
        rep = parse_reply(replies(rows)[-1])
        self.assertEqual(rep['state'], 3)       # failed
        self.assertEqual(rep['err'], 13)        # magic


class WebTest(unittest.TestCase):
    """docs/js/ota.js が同じ形を作るか (node があれば)。"""

    def setUp(self):
        if not os.path.exists(os.path.join(WEB, 'ota.js')):
            self.skipTest('docs/js/ota.js が無い')
        try:
            subprocess.run(['node', '--version'], capture_output=True, check=True)
        except Exception:
            self.skipTest('node が無い')

    def node_frames(self, data):
        """ota.js が作る 0xC3 レポート列 (16 進の list)。"""
        src = (
            "import { encodeImage } from '%s';\n"
            "const hex = process.argv[2];\n"
            "const bytes = Uint8Array.from(hex.match(/../g) || [],"
            " (h) => parseInt(h, 16));\n"
            "console.log(encodeImage(bytes).map("
            "(r) => Buffer.from(r).toString('hex')).join('\\n'));\n"
            % os.path.join(WEB, 'ota.js'))
        with tempfile.NamedTemporaryFile('w', suffix='.mjs', delete=False) as fh:
            fh.write(src)
            path = fh.name
        out = subprocess.run(['node', path, data.hex()], capture_output=True,
                             text=True)
        os.unlink(path)
        if out.returncode != 0:
            raise AssertionError('node が失敗:\n' + out.stderr)
        return [line for line in out.stdout.splitlines() if line]

    def test_browser_frames_are_accepted_by_the_device(self):
        """ブラウザが作った枠を、そのままデバイス側に食わせて通す。"""
        data = image(3000)
        frames = self.node_frames(data)
        self.assertEqual(len(frames), (len(data) + PAYLOAD - 1) // PAYLOAD)
        script = ('attach %d 4096\nbegin %d %s\n'
                  % (RING_MIN, len(data), hashlib.sha256(data).hexdigest()))
        for hexline in frames:
            self.assertEqual(len(hexline), REPORT * 2)
            script += 'raw %s\n' % hexline
        script += 'pump 65536\nend\nflash\n'
        rows = run(script)
        self.assertEqual(field(rows, 'end', 2), 'none')
        flash = [r for r in rows if r[0] == 'flash'][0]
        self.assertEqual(int(flash[1]), len(data))
        self.assertEqual(flash[2], hashlib.sha256(data).hexdigest())

    def test_browser_reads_the_device_reply(self):
        """デバイスが返した 32 バイトを ota.js が同じ意味に読むか。"""
        data = image(4000)
        rows = run(send_script(data, pump_every=0))
        rep = replies(rows)[0]
        mine = parse_reply(rep)
        src = (
            "import { parseStatusReport } from '%s';\n"
            "const hex = process.argv[2];\n"
            "const bytes = Uint8Array.from(hex.match(/../g) || [],"
            " (h) => parseInt(h, 16));\n"
            "console.log(JSON.stringify(parseStatusReport(bytes)));\n"
            % os.path.join(WEB, 'ota.js'))
        with tempfile.NamedTemporaryFile('w', suffix='.mjs', delete=False) as fh:
            fh.write(src)
            path = fh.name
        out = subprocess.run(['node', path, rep.hex()], capture_output=True,
                             text=True)
        os.unlink(path)
        self.assertEqual(out.returncode, 0, out.stderr)
        import json
        theirs = json.loads(out.stdout)
        self.assertEqual(theirs['accepted'], mine['accepted'])
        self.assertEqual(theirs['written'], mine['written'])
        self.assertEqual(theirs['size'], mine['size'])
        self.assertEqual(theirs['free'], mine['free'])
        self.assertEqual(theirs['rejected'], mine['rejected'])
        self.assertEqual(theirs['stateCode'], mine['state'])
        self.assertEqual(theirs['errCode'], mine['err'])

    def test_constants_match_between_c_and_js(self):
        """#define と JS の定数が 1 つでもずれたら落とす。"""
        with open(os.path.join(IDF, 'main/stackee_otacore.h')) as fh:
            header = fh.read()
        with open(os.path.join(WEB, 'ota.js')) as fh:
            js = fh.read()
        with open(os.path.join(IDF, 'main/stackee_otacore.c')) as fh:
            core = fh.read()
        for name, value in (('STACKEE_OTA_CMD_DATA', '0xC3'),
                            ('STACKEE_OTA_HEADER', '5'),
                            ('STACKEE_OTA_PAYLOAD', '27'),
                            ('STACKEE_OTA_REPORT', '32'),
                            ('STACKEE_OTA_ACK_EVERY', '1024')):
            self.assertIn(name, header)
        self.assertIn('#define STACKEE_OTA_PAYLOAD     27', header)
        self.assertIn('#define STACKEE_OTA_HEADER      5', header)
        self.assertIn('#define STACKEE_OTA_CMD_DATA    0xC3', header)
        self.assertIn('#define STACKEE_OTA_ACK_EVERY   1024', header)
        self.assertIn('#define STACKEE_OTA_CREDIT      (32u * 1024u)', header)
        self.assertIn('MAX_PAYLOAD: 27', js)
        self.assertIn('HEADER_SIZE: 5', js)
        self.assertIn('CMD_DATA: 0xc3', js)
        self.assertIn('ACK_EVERY: 1024', js)
        self.assertIn('CREDIT: 32 * 1024', js)
        # 状態名とエラー名の並び
        for name in ('none', 'busy', 'arg', 'nomem', 'flash_begin', 'offset',
                     'full', 'flash_write', 'flash_end', 'sha', 'size', 'idle',
                     'state', 'magic'):
            self.assertIn("'%s'" % name, js)
            self.assertIn('"%s"' % name, core)


class CommitOnlyTest(unittest.TestCase):
    """`node tools/ota.mjs --commit` を像なしで叩いたときの歩き方。

    ★ 2026-09-21 まで `--commit` は `--image` が無いと使い方を出すだけで、
      `--no-commit` で書き終えたあと**転送し直さないと切り替えられなかった**。
      ここは実機にも node-hid にも触らない。偽の link を渡して、
      `commitWritten()` が何を聞いて何を断るかだけを見る。
    """

    OTA_MJS = os.path.join(HERE, 'ota.mjs')

    def setUp(self):
        try:
            subprocess.run(['node', '--version'], capture_output=True, check=True)
        except Exception:
            self.skipTest('node が無い')

    def drive(self, replies, force=False):
        """偽の link で commitWritten を回し、{calls, result|error} を返す。

        replies は cmd -> 応答の list (呼ばれた順に取り出す)。
        """
        import json
        src = (
            "import { commitWritten } from '%s';\n"
            "const plan = JSON.parse(process.argv[2]);\n"
            "const force = process.argv[3] === '1';\n"
            "const calls = [];\n"
            "const link = {\n"
            "  request(cmd) {\n"
            "    calls.push(cmd);\n"
            "    const q = plan[cmd];\n"
            "    if (!q || !q.length) throw new Error('unexpected ' + cmd);\n"
            "    return Promise.resolve(q.length > 1 ? q.shift() : q[0]);\n"
            "  },\n"
            "};\n"
            "commitWritten(link, { force, sleep: () => Promise.resolve(),\n"
            "                      onStep: (k) => calls.push('step:' + k) })\n"
            "  .then((r) => console.log(JSON.stringify(\n"
            "      { calls, result: r })))\n"
            "  .catch((e) => console.log(JSON.stringify(\n"
            "      { calls, error: e.code || 'unknown', message: e.message })));\n"
            % self.OTA_MJS)
        with tempfile.NamedTemporaryFile('w', suffix='.mjs', delete=False) as fh:
            fh.write(src)
            path = fh.name
        out = subprocess.run(['node', path, json.dumps(replies),
                              '1' if force else '0'],
                             capture_output=True, text=True)
        os.unlink(path)
        self.assertEqual(out.returncode, 0, out.stderr)
        return json.loads(out.stdout)

    @staticmethod
    def info(running, nxt):
        part = lambda label, sha: {'label': label, 'sha256': sha,
                                   'version': sha[:7]}
        return {'ok': 1, 'running': part('ota_1', running),
                'boot': part('ota_1', running), 'next': part('ota_0', nxt)}

    OLD = 'f6' + '0' * 62
    NEW = 'e9' + '9' * 62

    def test_it_commits_without_sending_the_image_again(self):
        got = self.drive({
            'app.info': [self.info(self.OLD, self.NEW),
                         self.info(self.NEW, self.OLD)],
            'ota.commit': [{'ok': 1, 'boot': 'ota_0', 'in_ms': 500}],
        })
        self.assertNotIn('error', got, got)
        # 聞いたのは app.info -> ota.commit -> app.info の 3 回だけ。
        # **像は 1 バイトも送っていない** (ota.begin も 0xC3 も無い)。
        self.assertEqual([c for c in got['calls'] if not c.startswith('step:')],
                         ['app.info', 'ota.commit', 'app.info'])
        self.assertTrue(got['result']['committed'])
        self.assertEqual(got['result']['wantImage'], self.NEW)
        self.assertEqual(got['result']['after']['running']['sha256'], self.NEW)

    def test_it_refuses_when_next_is_the_running_image(self):
        got = self.drive({'app.info': [self.info(self.OLD, self.OLD)]})
        self.assertEqual(got['error'], 'same')
        self.assertNotIn('ota.commit', got['calls'])

    def test_force_commits_even_when_next_is_the_same(self):
        got = self.drive({
            'app.info': [self.info(self.OLD, self.OLD),
                         self.info(self.OLD, self.OLD)],
            'ota.commit': [{'ok': 1, 'boot': 'ota_0'}],
        }, force=True)
        self.assertNotIn('error', got, got)
        self.assertIn('ota.commit', got['calls'])

    def test_it_reports_notready_instead_of_a_bare_failure(self):
        # 書いていない (ota.end が通っていない) ときの本体の断り方。
        got = self.drive({
            'app.info': [self.info(self.OLD, self.NEW)],
            'ota.commit': [{'error': 'notready',
                            'note': 'ota.end が通っていない'}],
        })
        self.assertEqual(got['error'], 'notready')
        self.assertIn('--image', got['message'])

    def test_it_refuses_an_image_without_app_info(self):
        got = self.drive({'app.info': [{'error': 'unsupported'}]})
        self.assertEqual(got['error'], 'unsupported')

    def test_it_refuses_when_next_has_no_label(self):
        got = self.drive({'app.info': [{'ok': 1, 'running': {'sha256': OLD_SHA},
                                        'next': None}]})
        self.assertEqual(got['error'], 'nonext')

    def test_it_checks_what_actually_booted(self):
        # 切り替えたのに別の像で起きたら verify で落とす。
        got = self.drive({
            'app.info': [self.info(self.OLD, self.NEW),
                         self.info(self.OLD, self.NEW)],
            'ota.commit': [{'ok': 1, 'boot': 'ota_0'}],
        })
        self.assertEqual(got['error'], 'verify')

    def test_the_usage_text_mentions_the_standalone_commit(self):
        with open(self.OTA_MJS) as fh:
            src = fh.read()
        self.assertIn('--commit    書いてある next へ切り替えて再起動する', src)
        self.assertIn('out.commitOnly = true', src)


OLD_SHA = 'f6' + '0' * 62


if __name__ == '__main__':
    unittest.main(verbosity=2)
