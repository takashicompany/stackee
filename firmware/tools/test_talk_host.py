#!/usr/bin/env python3
"""会話の状態機械 (main/stackee_talksm.c) を Mac 上で確かめる。**実機に触らない。**

**本物の状態機械**を Mac 用にビルドし、偽の時計・マイク・スピーカー・通信を
つないで台本を流す。見ているのは現行 CircuitPython 版 (stackee_talk.py) と
同じ約束:

  * 押している間だけ録音し、離すと送る。短すぎる録音は送らない (0.3 秒未満)
  * 受理 (202) のあと **1 秒おき** に GET /jobs/<id> を撃つ
  * 返答が出たら PCM を取りに行き、**一次回答が鳴り終わってから**鳴らす
  * 390 秒で応答待ちを諦める。15 秒でスピーカー待ちを諦める
  * 30 秒で録音を打ち切る。返答音声の受け皿はその 4 倍 (120 秒 = 3.84 MB)
  * どこで失敗してもマイクを戻し、idle に戻る
  * talk.inject は録音の道を通らず、送信から先は同じ道を歩く

  python3 firmware/tools/test_talk_host.py
"""
import os
import re
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
IDF = os.path.dirname(HERE)

SOURCES = ['stackee_talksm.c', 'stackee_jsonlite.c']
# ★ ホストビルドは **ASan + UBSan つき**で回す。段階 3 の登録簿の直列化に
# 入れ物の外へ書く欠陥があり (snprintf の戻り値を足し込んでいた)、
# これを入れて初めて落ちるようになった。実機では同じ欠陥が静かに
# 隣の領域を壊す。遅くなるのは 1 秒未満。
SANITIZE = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']

_BINARY = None

DONE_BODY = ('{"state":"done","reply":"こんにちはなのだ","audio_url":'
             '"/jobs/abc/audio","sample_rate":16000,"channels":1,'
             '"sample_width":2}')
ACCEPT_BODY = '{"id":"abc","status_url":"/jobs/abc"}'
# 字幕つきのサーバ (subtitles_url が増えるだけ。他は 1 文字も変わらない)。
DONE_SUBS_BODY = ('{"state":"done","reply":"こんにちはなのだ","audio_url":'
                  '"/jobs/abc/audio","subtitles_url":"/jobs/abc/subtitles",'
                  '"sample_rate":16000,"channels":1,"sample_width":2}')


def binary():
    global _BINARY
    if _BINARY is None:
        out = os.path.join(tempfile.mkdtemp(), 'talk')
        cmd = (['cc', '-O1', '-std=gnu11', '-Wall', '-Werror'] + SANITIZE + [
                '-I', os.path.join(IDF, 'main'), '-o', out,
                os.path.join(IDF, 'hostbuild/talk_main.c')]
               + [os.path.join(IDF, 'main', s) for s in SOURCES])
        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError('ホストビルドに失敗:\n' + build.stderr)
        _BINARY = out
    return _BINARY


# ★ 短押し / 無音の切り捨て (2026-09-21) は既定で「1000 ms 以上 かつ
#   20 ms 窓の RMS 最大 ≥ 1500」。この木の台本はどれも `mic 40` + `t 400` で
#   **997 ms** しか録らないので、そのままだと全部「短すぎ」で捨てられる。
#   台本が `gate` を書いていなければ、**切り捨てを入れる前と同じ規則**
#   (300 ms の下限だけ・声は見ない) に戻してから流す。会話の状態機械を
#   見ている検査の意味を変えないため。
#   切り捨てそのもの (既定値を含む) は RecordGateTest が見る。
LEGACY_GATE = 'gate 300 0\n'


def run(script, legacy_gate=True):
    if legacy_gate and 'gate ' not in script:
        script = LEGACY_GATE + script
    out = subprocess.run([binary()], input=script, capture_output=True, text=True)
    if out.returncode != 0:
        raise AssertionError('実行に失敗:\n' + out.stderr)
    return out.stdout


def states(text):
    return [(name, int(ms)) for name, ms in
            re.findall(r'^STATE (\S+) (\d+)$', text, re.M)]


def state_names(text):
    return [s for s, _ in states(text)]


def http_calls(text):
    return re.findall(r'^HTTP (\S+) (\S+) (\d+) (\d+)$', text, re.M)


def last_print(text):
    rows = prints(text)
    assert rows, text
    return rows[-1]


def prints(text):
    """print 命令の行を全部 (途中の様子を見たいときに使う)。"""
    rows = re.findall(r'^NOW (\d+) STATE (\S+) POLLS (\d+) ALLOC (\d+) '
                      r'RELEASE (\d+) PAGES (-?\d+) PAGE (-?\d+) DROPPED (\d+) '
                      r'SUBBYTES (\d+) SRC (\S+) '
                      r'SHORT (\d+) SILENT (\d+) RECMS (\d+) RMSMAX (\d+) '
                      r'MINMS (\d+) VOICERMS (\d+) '
                      r'REPLY (.*) ERROR (.*)$', text, re.M)
    keys = ('now', 'state', 'polls', 'alloc', 'release', 'pages', 'page',
            'dropped', 'sub_bytes', 'src',
            'dropped_short', 'dropped_silent', 'rec_ms', 'rms_max',
            'min_ms', 'voice_rms', 'reply', 'error')
    out = []
    for row in rows:
        item = dict(zip(keys, row))
        for k in ('now', 'polls', 'alloc', 'release', 'pages', 'page',
                  'dropped', 'sub_bytes',
                  'dropped_short', 'dropped_silent', 'rec_ms', 'rms_max',
                  'min_ms', 'voice_rms'):
            item[k] = int(item[k])
        out.append(item)
    return out


def subtitles(text):
    """SUB 行 (帯に渡した文字列の並び)。"-" は「帯を消した」。

    ★ 帯は 3 行。ページは頭から 1 行ずつ**積む**ので、行は
      `\n` (逆斜線 + n) 区切りで 1 行に出てくる。
    """
    return re.findall(r'^SUB (.*)$', text, re.M)


def bands(text):
    """BAND 行 (ページ番号 -> 行数, 帯の中身)。"""
    return [(int(p), int(n), t)
            for p, n, t in re.findall(r'^BAND (\d+) (\d+) (.*)$', text, re.M)]


class HappyPathTest(unittest.TestCase):
    SCRIPT = """
ackms 800
respdelay 50
resp 202 %s
resp 200 {"state":"processing"}
resp 200 %s
resp 200 PCM:8000
mic 40
press
t 600
release
t 6000
print
""" % (ACCEPT_BODY, DONE_BODY)

    def setUp(self):
        self.out = run(self.SCRIPT)

    def test_state_order(self):
        self.assertEqual(
            state_names(self.out),
            ['idle', 'recording', 'upload', 'poll_wait', 'poll', 'poll_wait',
             'poll', 'audio', 'play_wait', 'playing', 'idle'])

    def test_polls_are_one_second_apart(self):
        times = dict()
        for name, ms in states(self.out):
            times.setdefault(name, []).append(ms)
        polls = times['poll']
        self.assertEqual(len(polls), 2)
        # ★ 1 秒の起点は「応答が返ってきた時刻」(現行 stackee_talk.py が
        #   self.polled をそこで置き直すのと同じ)。台本の respdelay は 50 ms。
        self.assertEqual(polls[1] - polls[0], 1000 + 50)

    def test_requests(self):
        calls = http_calls(self.out)
        self.assertEqual([c[0] for c in calls], ['POST', 'GET', 'GET', 'GET'])
        # 返答待ちの GET にだけ "?wait=" が付く (中継のロングポーリング)。
        self.assertEqual([c[1] for c in calls],
                         ['/talk', '/jobs/abc?wait=25', '/jobs/abc?wait=25',
                          '/jobs/abc/audio'])
        # POST の本体は WAV ヘッダ 44 バイト + PCM (16bit)。
        body = int(calls[0][2])
        self.assertEqual((body - 44) % 2, 0)
        self.assertGreater(body - 44, 16000 * 2 * 3 // 10)   # 0.3 秒ぶん以上
        # 返答 PCM の上限は **120 秒ぶん** (録音の 30 秒とは別の値)。
        self.assertEqual(int(calls[3][3]), 16000 * 2 * 120)
        self.assertEqual(int(calls[3][3]), 3840000)
        # 受理 (POST) は JSON ぶんのまま。返答待ちの GET は done に字幕の本文
        # (4 KB) が混ざるので 8192 + 2 x 4096 = 16384 (README §23-1)。
        self.assertEqual([int(c[3]) for c in calls[:3]], [8192, 16384, 16384])

    def test_reply_text_reaches_the_screen(self):
        self.assertIn('SHOW こんにちはなのだ', self.out)
        self.assertEqual(last_print(self.out)['reply'], 'こんにちはなのだ')

    def test_mic_is_restored_exactly_once(self):
        self.assertEqual(self.out.count('REC begin'), 1)
        self.assertEqual(self.out.count('REC end'), 1)

    def test_turn_timing_is_logged(self):
        self.assertIn('[talk-turn-timing]', self.out)


class AckExclusionTest(unittest.TestCase):
    """最終回答は一次回答が鳴り終わってから。現行 stackee_talk.py の play_wait。"""

    def test_final_audio_waits_for_the_local_phrase(self):
        out = run("""
ackms 5000
respdelay 10
resp 202 %s
resp 200 %s
resp 200 PCM:1600
mic 40
press
t 400
release
t 12000
print
""" % (ACCEPT_BODY, DONE_BODY))
        ack_done = out.index('ACK done')
        play = out.index('PLAY 1600')
        self.assertLess(ack_done, play, out)
        # 返答 PCM は一次回答の最中に受け取り終えている (待っているのは再生だけ)。
        audio_at = dict(states(out))['audio']
        play_at = dict(states(out))['playing']
        self.assertLess(audio_at, play_at)

    def test_no_local_phrase_means_no_wait(self):
        out = run("""
ack 0
respdelay 10
resp 202 %s
resp 200 %s
resp 200 PCM:1600
mic 40
press
t 400
release
t 3000
""" % (ACCEPT_BODY, DONE_BODY))
        self.assertIn('ACK skip', out)
        self.assertIn('PLAY 1600', out)


class LongReplyTest(unittest.TestCase):
    """返答音声は録音より長くてよい (30 秒 → 120 秒)。

    録音の上限 (30 秒) は変えていない。返答の受け皿はそれとは別物で、
    通信側が 1 往復ごとに取る受信バッファ (120 秒 = 3,840,000 バイト)。
    ここでは 50 秒ぶんの PCM を最後まで鳴らし切るかを見る。
    """

    def test_a_fifty_second_reply_plays_to_the_end(self):
        samples = 16000 * 50
        out = run("""
ack 0
respdelay 10
resp 202 %s
resp 200 %s
resp 200 PCM:%d
mic 40
press
t 400
release
t 60000
print
""" % (ACCEPT_BODY, DONE_BODY, samples))
        self.assertIn('PLAY %d' % samples, out)
        self.assertIn('PLAY done', out)
        self.assertEqual(state_names(out)[-1], 'idle')
        # 録音の上限を返答に流用していないこと (30 秒 = 480,000 サンプル)。
        self.assertGreater(samples, 16000 * 30)
        self.assertEqual(last_print(out)['error'], '')

    def test_the_turn_timing_log_survives_a_long_reply(self):
        """audio_duration_ms が 32 ビットであふれないか (120 秒で 1.92e9)。"""
        samples = 16000 * 120
        out = run("""
ack 0
respdelay 10
resp 202 %s
resp 200 %s
resp 200 PCM:%d
mic 40
press
t 400
release
t 123000
print
""" % (ACCEPT_BODY, DONE_BODY, samples))
        row = re.search(r'"audio_duration_ms":(-?\d+)', out)
        self.assertIsNotNone(row, out)
        self.assertEqual(int(row.group(1)), 120000)


class LongPollTest(unittest.TestCase):
    """返答待ちの GET は中継に握ってもらう (TLS の握手を 12 回から減らす)。

    見ているのは 2 つ:
      * 返答待ちの GET にだけ "?wait=25" が付く (POST と /audio には付かない)
      * その GET 自体に 1 秒以上かかったら、次はすぐ投げる (待ちを二重にしない)
    """

    SCRIPT = ('ackms 1\nrespdelay 1200\nresp 202 %s\n'
              'sticky 200 {"state":"processing"}\n'
              'mic 40\npress\nt 400\nrelease\nt 12000\nprint\n' % ACCEPT_BODY)

    def test_only_the_status_request_carries_the_wait_hint(self):
        out = run(self.SCRIPT)
        paths = [path for method, path, _, _ in http_calls(out)]
        self.assertEqual(paths[0], '/talk')
        self.assertTrue(all(p == '/jobs/abc?wait=25' for p in paths[1:]), paths)

    def test_a_slow_status_request_is_not_followed_by_another_wait(self):
        out = run(self.SCRIPT)
        polls = [ms for name, ms in states(out) if name == 'poll']
        self.assertGreater(len(polls), 3, out)
        # 1 往復 1200 ms。ロングポーリングが効いているので 1 秒の再送間隔は
        # 足さない (足すと 2200 ms おきになる)。
        gaps = [b - a for a, b in zip(polls, polls[1:])]
        self.assertTrue(all(1200 <= g <= 1210 for g in gaps), gaps)

    def test_a_fast_status_request_still_waits_a_second(self):
        # 中継が古くて即返る場合は従来どおり 1 秒あける (撃ちっぱなしにしない)。
        out = run('ackms 1\nrespdelay 50\nresp 202 %s\n'
                  'sticky 200 {"state":"processing"}\n'
                  'mic 40\npress\nt 400\nrelease\nt 12000\nprint\n' % ACCEPT_BODY)
        polls = [ms for name, ms in states(out) if name == 'poll']
        gaps = [b - a for a, b in zip(polls, polls[1:])]
        self.assertTrue(all(g == 1050 for g in gaps), gaps)


class ShortRecordingTest(unittest.TestCase):
    def test_under_three_tenths_of_a_second_is_not_sent(self):
        out = run("""
gate 300 0
mic 40
press
t 100
release
t 100
print
""")
        self.assertEqual(http_calls(out), [])
        self.assertIn('短すぎのため破棄', out)
        self.assertEqual(last_print(out)['state'], 'idle')
        self.assertEqual(out.count('REC end'), 1)


class RecordGateTest(unittest.TestCase):
    """短押し / 無音は**何も起こさない** (2026-09-21)。

    誤ってキーに触れただけで一次回答が鳴り、送信まで走るのを止める。
    録音を終えた時点で 2 つとも満たしたときだけ先へ進む:

      (a) 録音の長さ ≥ min_ms      (既定 1000 ms。録音の長さ = 押していた長さ)
      (b) 20 ms 窓の RMS の最大 ≥ voice_rms (既定 1500)

    ★ 満たさないときは **HTTP も一次回答も無し**。表情も listening から
      直接 idle へ戻る (thinking を通らない)。
    """

    # ★ 偽のマイクは 1 周 (1 ms) に `mic` で指定したサンプル数を返すので、
    #   台本の 1 ms が録音の約 2.5 ms になる (mic 40 のとき)。下の `ms` は
    #   台本の時間で、録れる長さはその約 2.5 倍。合否は rec_ms で見る。
    def turn(self, extra='', ms=1200, level=None, gate=None):
        script = ''
        if gate is not None:
            script += 'gate %d %d\n' % gate
        if level is not None:
            script += 'miclevel %d\n' % level
        script += ('ack 0\nrespdelay 10\nresp 202 %s\nresp 200 %s\n'
                   'resp 200 PCM:48000\nmic 40\n' % (ACCEPT_BODY, DONE_BODY))
        script += extra
        script += 'press\nt %d\nrelease\nt %d\nprint\n' % (ms, ms + 3000)
        # ★ ここは切り捨てそのものを見るので、legacy の緩い閾値を被せない。
        return run(script, legacy_gate=False)

    # ---- 既定の閾値 ------------------------------------------------------
    def test_the_defaults_are_a_second_and_fifteen_hundred(self):
        out = self.turn(ms=1200)
        info = last_print(out)
        # 既定は像に焼いてある値 (stackee_talksm.h)。
        self.assertEqual(info['min_ms'], 1000)
        self.assertEqual(info['voice_rms'], 1500)
        self.assertEqual(info['state'], 'idle')
        # 既定のまま声ありなら従来どおり進む。
        self.assertTrue(http_calls(out))
        self.assertEqual(info['dropped_short'], 0)
        self.assertEqual(info['dropped_silent'], 0)

    def test_a_short_press_does_nothing_at_all(self):
        out = self.turn(ms=300)
        info = last_print(out)
        self.assertEqual(info['dropped_short'], 1)
        self.assertEqual(info['dropped_silent'], 0)
        self.assertEqual(info['state'], 'idle')
        # ★ 送らない・一次回答を鳴らさない。
        self.assertEqual(http_calls(out), [])
        self.assertNotIn('ACK ', out)
        self.assertIn('短すぎのため破棄', out)
        # 表情は listening から直接 idle (thinking を通らない)。
        self.assertEqual(state_names(out), ['idle', 'recording', 'idle'])

    def test_a_long_press_with_no_voice_does_nothing_either(self):
        out = self.turn(ms=1500, level=0)
        info = last_print(out)
        self.assertEqual(info['dropped_silent'], 1)
        self.assertEqual(info['dropped_short'], 0)
        self.assertGreaterEqual(info['rec_ms'], 1000)
        self.assertEqual(info['rms_max'], 0)
        self.assertEqual(http_calls(out), [])
        self.assertNotIn('ACK ', out)
        self.assertIn('無音のため破棄', out)
        self.assertEqual(state_names(out), ['idle', 'recording', 'idle'])

    def test_voice_just_over_the_line_goes_through(self):
        out = self.turn(ms=1500, level=1500)      # ちょうど閾値
        self.assertTrue(http_calls(out))
        self.assertEqual(last_print(out)['dropped_silent'], 0)

    def test_voice_just_under_the_line_is_dropped(self):
        out = self.turn(ms=1500, level=1499)
        self.assertEqual(http_calls(out), [])
        self.assertEqual(last_print(out)['dropped_silent'], 1)

    # ---- 設定で変えられる ------------------------------------------------
    def test_the_thresholds_come_from_the_settings(self):
        # 緩めれば通る。
        out = self.turn(ms=400, level=100, gate=(300, 50))
        self.assertTrue(http_calls(out))
        # 締めれば落ちる (台本 1500 ms = 録音およそ 3.7 秒)。
        out = self.turn(ms=1500, level=4000, gate=(9000, 1500))
        self.assertEqual(http_calls(out), [])
        info = last_print(out)
        self.assertEqual(info['dropped_short'], 1)
        self.assertLess(info['rec_ms'], 9000)
        out = self.turn(ms=1500, level=4000, gate=(1000, 8000))
        self.assertEqual(http_calls(out), [])
        self.assertEqual(last_print(out)['dropped_silent'], 1)

    def test_zero_turns_a_condition_off(self):
        # voice_rms = 0 なら声を見ない (無音でも通る)。
        out = self.turn(ms=1500, level=0, gate=(1000, 0))
        self.assertTrue(http_calls(out))
        # min_ms = 0 でも 0.3 秒の床 (サーバが断る長さ) は残る。
        out = self.turn(ms=100, level=4000, gate=(0, 0))
        self.assertEqual(http_calls(out), [])
        self.assertEqual(last_print(out)['dropped_short'], 1)

    # ---- 数え方 ----------------------------------------------------------
    def test_the_counters_add_up(self):
        script = ('ack 0\nrespdelay 10\nmic 40\nmiclevel 0\n'
                  'press\nt 300\nrelease\nt 500\n'      # 短すぎ
                  'press\nt 1500\nrelease\nt 500\n'     # 無音
                  'print\n')
        info = last_print(run(script, legacy_gate=False))
        self.assertEqual(info['dropped_short'], 1)
        self.assertEqual(info['dropped_silent'], 1)
        self.assertEqual(info['state'], 'idle')

    def test_inject_is_not_gated(self):
        """talk.inject (決まった PCM を流す道) は切り捨てを通らない。"""
        out = run('ack 0\nrespdelay 10\nresp 202 %s\nresp 200 %s\n'
                  'resp 200 PCM:48000\nmic 40\ninject 16000\n'
                  't 5000\nprint\n' % (ACCEPT_BODY, DONE_BODY),
                  legacy_gate=False)
        self.assertTrue(http_calls(out))
        info = last_print(out)
        self.assertEqual(info['dropped_short'], 0)
        self.assertEqual(info['dropped_silent'], 0)


class VoiceRmsTest(unittest.TestCase):
    """RMS の測り方そのもの (stackee_talk_voice_rms)。"""

    def rms(self, script):
        info = last_print(run(script + 'print\n', legacy_gate=False))
        return info['rms_max'], info['rec_ms']

    def test_a_silent_recording_measures_zero(self):
        rms, ms = self.rms('gate 0 0\nmic 40\nmiclevel 0\n'
                           'press\nt 1000\nrelease\nt 100\n')
        self.assertEqual(rms, 0)
        self.assertGreater(ms, 900)

    def test_a_square_wave_measures_its_amplitude(self):
        for level in (100, 832, 1500, 8000):
            rms, _ms = self.rms('gate 0 0\nmic 40\nmiclevel %d\n'
                                'press\nt 1000\nrelease\nt 100\n' % level)
            self.assertEqual(rms, level, '振幅 %d' % level)

    def test_a_recording_shorter_than_one_window_measures_zero(self):
        # 20 ms (320 サンプル) に満たなければ測りようがない。
        rms, _ms = self.rms('gate 0 0\nmic 40\nmiclevel 4000\n'
                            'press\nt 5\nrelease\nt 100\n')
        self.assertEqual(rms, 0)


class TimeoutTest(unittest.TestCase):
    def test_polling_gives_up_after_390_seconds(self):
        script = ['ackms 1', 'respdelay 10', 'resp 202 ' + ACCEPT_BODY,
                  'sticky 200 {"state":"processing"}',
                  'mic 40', 'press', 't 400', 'release', 't 395000', 'print']
        out = run('\n'.join(script) + '\n')
        info = last_print(out)
        self.assertEqual(info['state'], 'idle')
        self.assertIn('応答待ちがタイムアウト', info['error'])
        # 失敗したときもマイクを戻しに行く (現行 fail() の restore_mic と同じ)。
        self.assertEqual(out.count('REC begin'), 1)
        self.assertGreaterEqual(out.count('REC end'), 1)
        # 1 秒おきなので、諦めるまでにおよそ 390 回は撃っている。
        self.assertGreater(info['polls'], 380)
        self.assertLess(info['polls'], 400)

    def test_recording_stops_after_30_seconds(self):
        out = run("""
ackms 1
respdelay 10
resp 202 %s
mic 4
press
t 31000
print
""" % ACCEPT_BODY)
        recording = [ms for name, ms in states(out) if name == 'recording'][0]
        upload = [ms for name, ms in states(out) if name == 'upload'][0]
        self.assertLessEqual(upload - recording, 30002)
        self.assertGreaterEqual(upload - recording, 30000)

    def test_speaker_wait_times_out_after_15_seconds(self):
        # ★ 一次回答が鳴っている間は数えない (現行 stackee_talk.py と同じ)。
        #   数え始めるのは「鳴らし始められない」状態になってから。
        out = run("""
ackms 1
playblock
respdelay 10
resp 202 %s
resp 200 %s
resp 200 PCM:1600
mic 40
press
t 400
release
t 20000
print
""" % (ACCEPT_BODY, DONE_BODY))
        info = last_print(out)
        self.assertEqual(info['state'], 'idle')
        self.assertIn('スピーカー待機がタイムアウト', info['error'])


class FailureTest(unittest.TestCase):
    def test_no_network_is_refused_before_recording(self):
        out = run("net 0\npress\nt 10\nprint\n")
        self.assertEqual(last_print(out)['error'], 'Wi-Fi 未接続です')
        self.assertNotIn('REC begin', out)

    def test_http_error_returns_to_idle(self):
        out = run("""
ackms 1
respdelay 10
resperr
mic 40
press
t 400
release
t 2000
print
""")
        info = last_print(out)
        self.assertEqual(info['state'], 'idle')
        self.assertIn('通信に失敗', info['error'])

    def test_server_error_status(self):
        out = run("""
ackms 1
respdelay 10
resp 500 {"error":"boom"}
mic 40
press
t 400
release
t 2000
print
""")
        self.assertIn('サーバー HTTP 500', last_print(out)['error'])

    def test_ignored_reply_is_shown_and_not_played(self):
        out = run("""
ackms 1
respdelay 10
resp 202 %s
resp 200 {"state":"ignored","error":"音声を認識できませんでした"}
mic 40
press
t 400
release
t 4000
print
""" % ACCEPT_BODY)
        self.assertIn('SHOW 音声を認識できませんでした', out)
        self.assertNotIn('PLAY ', out)
        self.assertEqual(last_print(out)['state'], 'idle')

    def test_wrong_audio_format_is_refused(self):
        out = run("""
ackms 1
respdelay 10
resp 202 %s
resp 200 {"state":"done","reply":"x","audio_url":"/a","sample_rate":8000,"channels":1,"sample_width":2}
mic 40
press
t 400
release
t 4000
print
""" % ACCEPT_BODY)
        self.assertIn('返答の音声形式が違います', last_print(out)['error'])

    def test_bad_audio_url_is_refused(self):
        out = run("""
ackms 1
respdelay 10
resp 202 %s
resp 200 {"state":"done","reply":"x","audio_url":"//evil","sample_rate":16000,"channels":1,"sample_width":2}
mic 40
press
t 400
release
t 4000
print
""" % ACCEPT_BODY)
        self.assertIn('audio_url が不正', last_print(out)['error'])

    def test_odd_pcm_is_refused(self):
        out = run("""
ackms 1
respdelay 10
resp 202 %s
resp 200 %s
resp 200 {
mic 40
press
t 400
release
t 4000
print
""" % (ACCEPT_BODY, DONE_BODY))
        self.assertIn('返答の PCM が不正', last_print(out)['error'])

    def test_microphone_failure_restores_and_fails(self):
        out = run("""
ackms 1
mic -1
press
t 10
print
""")
        info = last_print(out)
        self.assertEqual(info['state'], 'idle')
        self.assertIn('マイクから音声を取得できません', info['error'])
        self.assertEqual(out.count('REC end'), 1)


class BufferLifetimeTest(unittest.TestCase):
    """録音の領域 (実機では 960 KB) を、どの終わり方でも必ず返すか。

    `talk_main.c` は本当に malloc する。ASan つきでビルドしているので、
    二重解放も使い終わったあとの参照もここで落ちる。確保した数と返した数が
    合っていること、そして必ず idle に戻ることを見る。
    """

    CASES = {
        'happy': 'ackms 10\nrespdelay 5\nresp 202 %s\nresp 200 %s\n'
                 'resp 200 PCM:1600\nmic 40\npress\nt 400\nrelease\nt 4000\nprint\n'
                 % (ACCEPT_BODY, DONE_BODY),
        'alloc_fail': 'allocfail\nmic 40\npress\nt 400\nrelease\nt 100\nprint\n',
        'mic_fail': 'mic -1\npress\nt 20\nprint\n',
        'too_short': 'mic 40\npress\nt 100\nrelease\nt 100\nprint\n',
        'post_error': 'ackms 10\nrespdelay 5\nresperr\nmic 40\npress\nt 400\n'
                      'release\nt 2000\nprint\n',
        'http_500': 'ackms 10\nrespdelay 5\nresp 500 {}\nmic 40\npress\nt 400\n'
                    'release\nt 2000\nprint\n',
        'bad_accept': 'ackms 10\nrespdelay 5\nresp 202 {"id":"x"}\nmic 40\npress\n'
                      't 400\nrelease\nt 2000\nprint\n',
        'ignored': 'ackms 10\nrespdelay 5\nresp 202 %s\n'
                   'resp 200 {"state":"ignored"}\nmic 40\npress\nt 400\nrelease\n'
                   't 4000\nprint\n' % ACCEPT_BODY,
        'poll_timeout': 'ackms 10\nrespdelay 5\nresp 202 %s\n'
                        'sticky 200 {"state":"processing"}\nmic 40\npress\nt 400\n'
                        'release\nt 395000\nprint\n' % ACCEPT_BODY,
        'inject': 'ackms 10\nrespdelay 5\nresp 202 %s\nresp 200 %s\n'
                  'resp 200 PCM:1600\ninject 16000\nt 4000\nprint\n'
                  % (ACCEPT_BODY, DONE_BODY),
        'inject_post_fails': 'ackms 10\nrespdelay 5\nresperr\ninject 16000\n'
                             't 2000\nprint\n',
    }

    def test_every_path_returns_the_buffer_and_ends_idle(self):
        for name, script in self.CASES.items():
            out = run(script)
            info = last_print(out)
            self.assertEqual(info['state'], 'idle', '%s: %s' % (name, out))
            self.assertEqual(info['alloc'], info['release'],
                             '%s: 確保 %d / 返却 %d' % (name, info['alloc'],
                                                       info['release']))


class InjectTest(unittest.TestCase):
    """talk.inject は録音の道を通らず、送信から先は同じ道を歩く。"""

    def test_inject_skips_recording(self):
        out = run("""
ackms 100
respdelay 10
resp 202 %s
resp 200 %s
resp 200 PCM:1600
inject 16000
t 4000
print
""" % (ACCEPT_BODY, DONE_BODY))
        self.assertIn('INJECT 1', out)
        self.assertNotIn('REC begin', out)
        self.assertEqual(state_names(out)[:3], ['idle', 'upload', 'poll_wait'])
        calls = http_calls(out)
        self.assertEqual(int(calls[0][2]), 44 + 16000 * 2)
        self.assertEqual(last_print(out)['state'], 'idle')

    def test_inject_is_refused_while_busy(self):
        out = run("""
ackms 100
respdelay 100000
resp 202 %s
inject 16000
t 100
inject 16000
t 10
""" % ACCEPT_BODY)
        self.assertEqual(re.findall(r'^INJECT (\d)$', out, re.M), ['1', '0'])

    def test_inject_needs_enough_samples(self):
        out = run("inject 100\nt 10\n")
        self.assertIn('INJECT 0', out)


def done_body(subtitles=None, url=True):
    """done の JSON。subtitles は **JSON に逃がしたまま**の 1 文字列。"""
    body = ('{"state":"done","reply":"こんにちはなのだ",'
            '"audio_url":"/jobs/abc/audio"')
    if url:
        body += ',"subtitles_url":"/jobs/abc/subtitles"'
    if subtitles is not None:
        body += ',"subtitles":"%s"' % subtitles
    return body + ',"sample_rate":16000,"channels":1,"sample_width":2}'


class InlineSubtitleTest(unittest.TestCase):
    r"""done の JSON に混ざってきた本文 ("subtitles") を使う。

    ★ 別 GET は実機で**約 8 秒**かかる (要求ごとに TLS を張り直すため。
      README §23-6 の実測)。本文は 4 KB 以下なので done に混ぜてもらえば、
      喋り始めがその 8 秒ぶん早い。見ているのは 4 つ:

      * 本文があれば `subs` 状態 (別 GET) を**通らない**
      * 本文が無ければ従来どおり `subtitles_url` を GET する (旧サーバ互換)
      * どちらも無ければ字幕なしで従来どおり鳴る
      * 本文が壊れていたら url へ落ちる。url も無ければ字幕なしで鳴る
    """

    INLINE = r'0\tこんにちは\n1000\tさようなら\n2000\tまたね\n'

    def turn(self, done, subs_resp=None, pcm=48000):
        script = 'ack 0\nrespdelay 10\nresp 202 %s\nresp 200 %s\n' % (
            ACCEPT_BODY, done)
        if subs_resp is not None:
            script += 'subs 200 %s\n' % subs_resp
        script += ('resp 200 PCM:%d\nmic 40\npress\nt 400\nrelease\n'
                   't 3000\nprint\nt 5000\nprint\n' % pcm)
        return run(script)

    def test_inline_skips_the_extra_request(self):
        out = self.turn(done_body(self.INLINE))
        self.assertNotIn('subs', state_names(out))
        self.assertEqual([c[1] for c in http_calls(out)],
                         ['/talk', '/jobs/abc?wait=25', '/jobs/abc/audio'])
        mid = prints(out)[0]
        self.assertEqual(mid['state'], 'playing')
        self.assertEqual(mid['pages'], 3)
        self.assertEqual(mid['src'], 'inline')
        self.assertEqual(mid['dropped'], 0)
        self.assertEqual(subtitles(out),
                         [r'こんにちは',
                          r'こんにちは\nさようなら',
                          r'こんにちは\nさようなら\nまたね', '-'])

    def test_the_status_request_asks_for_a_bigger_buffer(self):
        # done に 4 KB の本文が混ざるので 8 KB では足りない。
        out = self.turn(done_body(self.INLINE))
        calls = http_calls(out)
        self.assertEqual(int(calls[0][3]), 8192)             # POST は据え置き
        self.assertEqual(int(calls[1][3]), 8192 + 2 * 4096)  # 返答待ちの GET
        self.assertEqual(int(calls[1][3]), 16384)

    def test_tabs_and_newlines_come_back_out_of_the_json(self):
        out = self.turn(done_body(r'0\tタブ\tは本文に入らない\n'))
        # 2 つ目のタブから先も本文の一部。行は 1 つだけ採れる。
        self.assertEqual(prints(out)[0]['pages'], 1)
        self.assertEqual(subtitles(out)[0], 'タブ\tは本文に入らない')

    def test_other_escapes_survive(self):
        out = self.turn(done_body(r'0\t\"かぎ\" \\ と \/\n'))
        self.assertEqual(subtitles(out)[0], '"かぎ" \\ と /')

    def test_a_url_only_server_still_works(self):
        out = self.turn(done_body(None), subs_resp=self.INLINE)
        self.assertIn('subs', state_names(out))
        self.assertEqual([c[1] for c in http_calls(out)],
                         ['/talk', '/jobs/abc?wait=25', '/jobs/abc/subtitles',
                          '/jobs/abc/audio'])
        mid = prints(out)[0]
        self.assertEqual(mid['pages'], 3)
        self.assertEqual(mid['src'], 'url')

    def test_neither_means_no_subtitles(self):
        out = self.turn(done_body(None, url=False))
        self.assertNotIn('subs', state_names(out))
        self.assertEqual(subtitles(out), [])
        mid = prints(out)[0]
        self.assertEqual(mid['pages'], 0)
        self.assertEqual(mid['src'], 'none')
        self.assertEqual(mid['state'], 'playing')

    def test_a_broken_inline_body_falls_back_to_the_url(self):
        # 1 行も採れない本文。url があるならそちらへ落ちる。
        out = self.turn(done_body(r'こわれた\nぜんぶだめ\n'),
                        subs_resp=self.INLINE)
        self.assertIn('subs', state_names(out))
        mid = prints(out)[0]
        self.assertEqual(mid['pages'], 3)
        self.assertEqual(mid['src'], 'url')

    def test_a_broken_inline_body_without_a_url_still_plays(self):
        out = self.turn(done_body(r'こわれた\n', url=False))
        self.assertNotIn('subs', state_names(out))
        self.assertIn('PLAY 48000', out)
        info = last_print(out)
        self.assertEqual(info['state'], 'idle')
        self.assertEqual(info['error'], '')

    def test_a_non_string_subtitles_value_is_ignored(self):
        for value in ('123', 'null', '{"a":1}', '[]'):
            done = ('{"state":"done","reply":"x","audio_url":"/jobs/abc/audio",'
                    '"subtitles":%s,"sample_rate":16000,"channels":1,'
                    '"sample_width":2}' % value)
            out = self.turn(done)
            self.assertNotIn('subs', state_names(out), value)
            self.assertEqual(prints(out)[0]['src'], 'none', value)
            self.assertEqual(last_print(out)['error'], '', value)

    def test_the_page_limit_applies_to_the_inline_body_too(self):
        body = ''.join(r'%d\t%d\n' % (i * 10, i) for i in range(60))
        out = self.turn(done_body(body))
        mid = prints(out)[0]
        self.assertEqual(mid['pages'], 48)
        self.assertEqual(mid['dropped'], 60 - 48)
        self.assertEqual(mid['src'], 'inline')

    def test_an_over_long_line_is_dropped_whole(self):
        # 1 行ぶんの中継ぎ (128 B) を超える行は、改行まで捨てて次へ進む。
        out = self.turn(done_body(r'0\t' + 'あ' * 90 + r'\n1000\tつぎ\n'))
        mid = prints(out)[0]
        self.assertEqual(mid['pages'], 1)
        self.assertEqual(mid['dropped'], 1)
        self.assertEqual(subtitles(out)[0], 'つぎ')

    def test_a_four_kilobyte_body_is_read_whole(self):
        # 契約の上限に近い大きさ。48 ページ x 15 全角 (45 B)。
        lines = [r'%d\t%s' % (i * 1000, 'あ' * 15) for i in range(48)]
        out = self.turn(done_body('\\n'.join(lines) + r'\n'), pcm=16000 * 60)
        mid = prints(out)[0]
        self.assertEqual(mid['pages'], 48)
        self.assertEqual(mid['dropped'], 0)
        self.assertEqual(mid['src'], 'inline')
        # 48 行 x (桁数 + TAB + 45 B + 改行) = 2,500 B 前後
        self.assertGreater(mid['sub_bytes'], 2000)
        self.assertLessEqual(mid['sub_bytes'], 4096)
        self.assertEqual(subtitles(out)[0], 'あ' * 15)

    def test_the_turn_timing_log_says_where_the_subtitles_came_from(self):
        out = self.turn(done_body(self.INLINE))
        self.assertIn('"sub_src":"inline"', out)
        self.assertIn('"src":"inline"', out)


class SubtitleTest(unittest.TestCase):
    """返答音声の字幕 (scratchpad/subtitle_design.md)。

    見ているのは 5 つ:
      * `subtitles_url` があれば **/audio より先に** 4096 B 上限で取りに行く
      * 行 "<start_ms>\\t<text>" を読み、ページが変わった時だけ帯へ渡す
      * 再生が終われば帯を消す
      * 取得や解析に失敗しても会話は止まらない (字幕なしで鳴る)
      * 旧サーバ (`subtitles_url` 無し) では 1 手も増えない
    """

    SUBS = r'0\tこんにちは\n1000\tさようなら\n2000\tまたね\n'

    def happy(self, subs=None, status=200):
        return run("""
ack 0
respdelay 10
resp 202 %s
resp 200 %s
subs %d %s
resp 200 PCM:48000
mic 40
press
t 400
release
t 3000
print
t 5000
print
""" % (ACCEPT_BODY, DONE_SUBS_BODY, status,
       self.SUBS if subs is None else subs))

    def test_subtitles_are_fetched_before_the_audio(self):
        out = self.happy()
        calls = http_calls(out)
        self.assertEqual([c[1] for c in calls],
                         ['/talk', '/jobs/abc?wait=25', '/jobs/abc/subtitles',
                          '/jobs/abc/audio'])
        # 字幕の受け皿は固定の 4096 B (返答 PCM の 3.84 MB とは別)。
        self.assertEqual(int(calls[2][3]), 4096)
        self.assertIn('subs', state_names(out))

    def test_pages_are_shown_in_order_and_cleared_at_the_end(self):
        out = self.happy()
        # ★ 行は積む。2 ページ目は 1 ページ目の下に足す (置き換えない)。
        self.assertEqual(subtitles(out),
                         [r'こんにちは',
                          r'こんにちは\nさようなら',
                          r'こんにちは\nさようなら\nまたね', '-'])
        # ★ 3 ページちょうどで帯が埋まる (4 ページ目があれば頁がめくれる)。
        # 再生の途中では 3 ページを持っている。
        mid = prints(out)[0]
        self.assertEqual(mid['state'], 'playing')
        self.assertEqual(mid['pages'], 3)
        self.assertEqual(mid['dropped'], 0)
        self.assertEqual(mid['page'], 1)        # 再生 1.5 秒すぎ = 2 ページ目
        # 鳴り終われば帯も入れ物も空にする。
        info = last_print(out)
        self.assertEqual(info['state'], 'idle')
        self.assertEqual(info['pages'], 0)
        self.assertEqual(info['page'], -1)

    def test_a_page_is_pushed_only_when_it_changes(self):
        # 3 ページで 3 回 + 消すので 1 回。毎周呼んでいたら数千回になる。
        out = self.happy()
        self.assertEqual(len(subtitles(out)), 4)

    def test_pages_change_at_the_start_times(self):
        out = self.happy()
        lines = out.splitlines()
        play_at = [i for i, s in enumerate(lines) if s.startswith('PLAY ')][0]
        shown = [i for i, s in enumerate(lines) if s.startswith('SUB ')]
        self.assertTrue(all(i > play_at for i in shown[:3]), out[:400])

    def test_ms_to_page_index(self):
        out = run("""
ack 0
respdelay 10
resp 202 %s
resp 200 %s
subs 200 %s
resp 200 PCM:48000
mic 40
press
t 400
release
t 2000
pageat 0
pageat 999
pageat 1000
pageat 1999
pageat 2000
pageat 60000
""" % (ACCEPT_BODY, DONE_SUBS_BODY, self.SUBS))
        rows = re.findall(r'^PAGEAT (\d+) (-?\d+) (.*)$', out, re.M)
        self.assertEqual([(int(ms), int(i)) for ms, i, _ in rows],
                         [(0, 0), (999, 0), (1000, 1), (1999, 1),
                          (2000, 2), (60000, 2)])

    def test_bad_lines_are_ignored(self):
        # タブ無し・数字でない・本文が空・空行はどれも捨てる。
        out = self.happy(subs=r'0\tよい\nこわれた行\nx\tだめ\n900\t\n\n1200\tもうひとつ\n')
        mid = prints(out)[0]
        self.assertEqual(mid['pages'], 2)
        self.assertEqual(mid['dropped'], 4)
        self.assertEqual(subtitles(out), [r'よい', r'よい\nもうひとつ', '-'])

    def test_too_many_pages_are_dropped(self):
        body = ''.join(r'%d\t%d\n' % (i * 10, i) for i in range(60))
        out = self.happy(subs=body)
        mid = prints(out)[0]
        self.assertEqual(mid['pages'], 48)           # STACKEE_TALK_SUB_PAGES
        self.assertEqual(mid['dropped'], 60 - 48)

    def test_lines_stack_until_the_band_is_full_then_the_page_turns(self):
        """帯は 3 行。4 ページ目で帯を空にして 1 行目に置く。

        ★ 行の集合はページ番号だけで決まる (i%3 行目に出る)。
          ここは state machine を通さずに stackee_talk_band を直に呼ぶ。
        """
        body = ''.join(r'%d\tぺ%d\n' % (i * 1000, i) for i in range(9))
        out = self.happy(subs=body) + run("""
ack 0
respdelay 10
resp 202 %s
resp 200 %s
subs 200 %s
resp 200 PCM:48000
mic 40
press
t 400
release
t 2000
band 0
band 1
band 2
band 3
band 4
band 6
band 8
band 9
""" % (ACCEPT_BODY, DONE_SUBS_BODY, body))
        rows = bands(out)
        self.assertEqual(rows, [
            (0, 1, r'ぺ0'),
            (1, 2, r'ぺ0\nぺ1'),
            (2, 3, r'ぺ0\nぺ1\nぺ2'),
            (3, 1, r'ぺ3'),                 # 頁めくり
            (4, 2, r'ぺ3\nぺ4'),
            (6, 1, r'ぺ6'),                 # 2 回目の頁めくり
            (8, 3, r'ぺ6\nぺ7\nぺ8'),
            (9, 0, '-'),                    # ページが無い
        ])

    def test_the_band_never_carries_more_than_three_lines(self):
        body = ''.join(r'%d\tぺ%d\n' % (i * 1000, i) for i in range(9))
        out = self.happy(subs=body)
        for text in subtitles(out):
            if text == '-':
                continue
            self.assertLessEqual(len(text.split(r'\n')), 3, text)

    def test_a_missing_subtitle_file_does_not_stop_the_reply(self):
        out = self.happy(status=404)
        self.assertIn('PLAY 48000', out)
        self.assertEqual(last_print(out)['state'], 'idle')
        self.assertEqual(last_print(out)['error'], '')
        self.assertEqual(subtitles(out), [])

    def test_a_broken_connection_while_fetching_subtitles_is_survivable(self):
        out = run("""
ack 0
respdelay 10
resp 202 %s
resp 200 %s
resperr
resp 200 PCM:16000
mic 40
press
t 400
release
t 3000
print
""" % (ACCEPT_BODY, DONE_SUBS_BODY))
        self.assertIn('PLAY 16000', out)
        info = last_print(out)
        self.assertEqual(info['state'], 'idle')
        self.assertEqual(info['error'], '')
        self.assertEqual(info['pages'], 0)

    def test_an_old_server_behaves_exactly_as_before(self):
        out = run("""
ack 0
respdelay 10
resp 202 %s
resp 200 %s
resp 200 PCM:16000
mic 40
press
t 400
release
t 3000
print
""" % (ACCEPT_BODY, DONE_BODY))
        self.assertNotIn('subs', state_names(out))
        self.assertEqual([c[1] for c in http_calls(out)],
                         ['/talk', '/jobs/abc?wait=25', '/jobs/abc/audio'])
        self.assertEqual(subtitles(out), [])

    def test_a_bad_subtitles_url_is_ignored(self):
        out = run("""
ack 0
respdelay 10
resp 202 %s
resp 200 {"state":"done","reply":"x","audio_url":"/jobs/abc/audio","subtitles_url":"//evil","sample_rate":16000,"channels":1,"sample_width":2}
resp 200 PCM:16000
mic 40
press
t 400
release
t 3000
print
""" % ACCEPT_BODY)
        self.assertNotIn('subs', state_names(out))
        self.assertEqual(last_print(out)['state'], 'idle')

    def test_the_turn_timing_log_carries_the_page_count(self):
        out = self.happy()
        row = re.search(r'"sub_pages":(\d+)', out)
        self.assertIsNotNone(row, out)
        self.assertEqual(int(row.group(1)), 3)

    def test_a_failed_turn_clears_the_band(self):
        # 再生の途中で失敗させる道が無いので、鳴らし始められない場合を見る。
        out = run("""
ack 0
playblock
respdelay 10
resp 202 %s
resp 200 %s
subs 200 %s
resp 200 PCM:16000
mic 40
press
t 400
release
t 20000
print
""" % (ACCEPT_BODY, DONE_SUBS_BODY, self.SUBS))
        info = last_print(out)
        self.assertEqual(info['state'], 'idle')
        self.assertIn('スピーカー待機がタイムアウト', info['error'])
        self.assertEqual(info['pages'], 0)           # 後始末で捨ててある


if __name__ == '__main__':
    unittest.main(verbosity=2)
