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
  * 画像 (POST /look、image/jpeg) は受理から先が会話と同じ道。撮影の前に
    押さえ、その間と往復の間は会話キーを受け付けない。play=0 は鳴らさない
  * CSTM_0〜9 (POST /key) は GET /inbox で seq を得てから送る。ignored は
    音なしで「未設定」、prompt は /look と同じ後半、command は受け箱を回して
    発話を鳴らす (字幕だけのものは音なし)。会話・画像との排他は同じ

  python3 firmware/tools/test_talk_host.py
"""
import os
import re
from pathlib import Path
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
LEGACY_GATE = 'gate 300 0 0\n'


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
                      r'RMS2ND (\d+) LOUD (\d+) GUIDE (\d+) '
                      r'MINMS (\d+) VOICERMS (\d+) '
                      r'REPLY (.*) ERROR (.*)$', text, re.M)
    keys = ('now', 'state', 'polls', 'alloc', 'release', 'pages', 'page',
            'dropped', 'sub_bytes', 'src',
            'dropped_short', 'dropped_silent', 'rec_ms', 'rms_max',
            'rms_2nd', 'loud', 'guide', 'min_ms', 'voice_rms', 'reply', 'error')
    out = []
    for row in rows:
        item = dict(zip(keys, row))
        for k in ('now', 'polls', 'alloc', 'release', 'pages', 'page',
                  'dropped', 'sub_bytes',
                  'dropped_short', 'dropped_silent', 'rec_ms', 'rms_max',
                  'rms_2nd', 'loud', 'guide', 'min_ms', 'voice_rms'):
            item[k] = int(item[k])
        out.append(item)
    return out


def subtitles(text):
    """SUB 行 (帯に渡した文字列の並び)。"-" は「帯を消した」。

    ★ 帯は 3 行。ページは頭から 1 行ずつ**積む**ので、行は
      `\n` (逆斜線 + n) 区切りで 1 行に出てくる。
    """
    return re.findall(r'^SUB (.*)$', text, re.M)


# 案内の字幕 (2026-09-22)。会話の状態をそのまま言葉にして帯へ出す。
GUIDE_REC = r'マイクに向かって\n話しかけてください'
GUIDE_THINK = '考えています…'


def reply_subs(text):
    """SUB 行から**案内を除いた**もの (返答の字幕だけを見たいとき)。"""
    return [s for s in subtitles(text) if s not in (GUIDE_REC, GUIDE_THINK)]


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
            script += 'gate %s\n' % ' '.join(str(v) for v in gate)
        if level is not None:
            script += 'miclevel %d\n' % level
        script += ('ack 0\nrespdelay 10\nresp 202 %s\nresp 200 %s\n'
                   'resp 200 PCM:48000\nmic 40\n' % (ACCEPT_BODY, DONE_BODY))
        script += extra
        script += 'press\nt %d\nrelease\nt %d\nprint\n' % (ms, ms + 3000)
        # ★ ここは切り捨てそのものを見るので、legacy の緩い閾値を被せない。
        return run(script, legacy_gate=False)

    # ---- 既定の閾値 ------------------------------------------------------
    def test_the_defaults_are_a_second_and_a_thousand(self):
        out = self.turn(ms=1200)
        info = last_print(out)
        # 既定は像に焼いてある値 (stackee_talksm.h)。
        self.assertEqual(info['min_ms'], 1000)
        self.assertEqual(info['voice_rms'], 1000)
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
        out = self.turn(ms=1500, level=1000)      # ちょうど閾値
        self.assertTrue(http_calls(out))
        self.assertEqual(last_print(out)['dropped_silent'], 0)

    def test_voice_just_under_the_line_is_dropped(self):
        out = self.turn(ms=1500, level=999)
        self.assertEqual(http_calls(out), [])
        self.assertEqual(last_print(out)['dropped_silent'], 1)

    # ---- 立ち上がりの跳ね -------------------------------------------------
    def test_a_single_loud_window_is_not_voice(self):
        """★ マイクを開けた直後の跳ね 1 窓では「声あり」にしない。

        実測 (2026-09-21): 環境音しか無い部屋でも **窓 5 の RMS が
        3,257〜3,945** になる (6 回測って毎回 窓 5)。窓の平均は 270 前後。
        「いちばん大きい窓」で判定すると、この跳ねだけで必ず通ってしまう。
        """
        out = self.turn(ms=1500, level=100, extra='burst 1 4000\n')
        info = last_print(out)
        self.assertGreater(info['rms_max'], 3000)     # 跳ねは見えている
        self.assertEqual(info['loud'], 1)             # でも 1 窓だけ
        self.assertEqual(info['dropped_silent'], 1)
        self.assertEqual(http_calls(out), [])

    def test_four_loud_windows_are_still_not_enough(self):
        out = self.turn(ms=1500, level=100, extra='burst 4 4000\n')
        self.assertEqual(last_print(out)['loud'], 4)
        self.assertEqual(http_calls(out), [])

    def test_five_loud_windows_are_voice(self):
        # 100 ms 続けば声。
        out = self.turn(ms=1500, level=100, extra='burst 5 4000\n')
        info = last_print(out)
        self.assertEqual(info['loud'], 5)
        self.assertEqual(info['dropped_silent'], 0)
        self.assertTrue(http_calls(out))

    def test_the_second_loudest_window_is_reported(self):
        # 跳ね 1 窓 + 静かな残り → 2 番目は静かな窓の値。
        out = self.turn(ms=1500, level=100, extra='burst 1 4000\n')
        info = last_print(out)
        self.assertGreater(info['rms_max'], 3000)
        self.assertEqual(info['rms_2nd'], 100)

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


class GuideTest(unittest.TestCase):
    """案内の字幕 (2026-09-22)。会話の状態をそのまま言葉にして帯へ出す。

      録音中 … 「マイクに向かって話しかけてください」(15 桁で 2 行)
      考え中 … 「考えています…」

    ★ **帯の持ち主は 1 人。** 一次回答 (ack) が鳴っている間と、返答の字幕が
      出ている間は案内を出さない。持ち主が居るときは消しもしない。
    """

    SUBS = r'0\tこんにちは\n1000\tさようなら\n'

    def turn(self, ack_ms=0, subs=None, ms=1200, level=None, gate=None):
        script = ''
        if gate is not None:
            script += 'gate %s\n' % ' '.join(str(v) for v in gate)
        if level is not None:
            script += 'miclevel %d\n' % level
        script += 'ack %d\nackms %d\nrespdelay 10\n' % (1 if ack_ms else 0,
                                                          ack_ms)
        script += 'resp 202 %s\nresp 200 %s\n' % (
            ACCEPT_BODY, DONE_SUBS_BODY if subs is not None else DONE_BODY)
        if subs is not None:
            script += 'subs 200 %s\n' % subs
        script += 'resp 200 PCM:48000\nmic 40\n'
        script += 'press\nt %d\nrelease\nt %d\nprint\n' % (ms, ms + 6000)
        return run(script, legacy_gate=False)

    # ---- 録音中 ----------------------------------------------------------
    def test_the_recording_guide_appears_while_the_key_is_held(self):
        out = self.turn(subs=self.SUBS)
        self.assertEqual(subtitles(out)[0], GUIDE_REC)
        # 押している間に出る (録音が終わる前)。
        lines = out.splitlines()
        first = [i for i, s in enumerate(lines) if s.startswith('SUB ')][0]
        rec_end = [i for i, s in enumerate(lines) if s == 'REC end'][0]
        self.assertLess(first, rec_end)

    def test_the_recording_guide_is_two_lines_of_fifteen_columns(self):
        # サーバと同じ規則で割ってある (tools/ack_lines.py と突き合わせる)。
        import ack_lines
        self.assertEqual(GUIDE_REC.replace('\\n', '\n').split('\n'),
                         ack_lines.subtitle_lines('マイクに向かって話しかけてください'))
        for line in GUIDE_REC.replace('\\n', '\n').split('\n'):
            self.assertLessEqual(ack_lines.page_width(line), 15)

    # ---- 考え中 ----------------------------------------------------------
    def test_the_thinking_guide_follows_the_recording_one(self):
        out = self.turn(subs=self.SUBS)
        subs = subtitles(out)
        self.assertEqual(subs[0], GUIDE_REC)
        self.assertEqual(subs[1], GUIDE_THINK)

    def test_the_thinking_guide_waits_for_the_opener(self):
        """★ 一次回答が鳴っている間は案内を出さない (鳴り終わってから)。"""
        out = self.turn(ack_ms=800, subs=self.SUBS)
        subs = subtitles(out)
        # 録音中の案内のあと、ack が鳴っている間は 1 つも出ない。
        self.assertEqual(subs[0], GUIDE_REC)
        self.assertEqual(subs[1], GUIDE_THINK)
        lines = out.splitlines()
        ack_at = [i for i, s in enumerate(lines) if s.startswith('ACK ')][0]
        think_at = [i for i, s in enumerate(lines)
                    if s == 'SUB ' + GUIDE_THINK][0]
        # 「考えています…」は ack が始まったあと。
        self.assertGreater(think_at, ack_at)

    def test_the_reply_subtitles_replace_the_thinking_guide(self):
        out = self.turn(subs=self.SUBS)
        subs = subtitles(out)
        self.assertEqual(subs[:2], [GUIDE_REC, GUIDE_THINK])
        # そのあとは返答の字幕。案内は出てこない。
        self.assertEqual(subs[2:], [r'こんにちは', r'こんにちは\nさようなら', '-'])

    def test_the_guide_is_gone_when_the_turn_ends(self):
        out = self.turn(subs=self.SUBS)
        self.assertEqual(subtitles(out)[-1], '-')
        self.assertEqual(last_print(out)['guide'], 0)   # GUIDE_NONE

    # ---- 破棄 ------------------------------------------------------------
    def test_a_discarded_recording_clears_the_guide(self):
        """短押し / 無音で捨てたら案内も消す (帯は黒のまま文字なし)。"""
        out = self.turn(ms=300, subs=self.SUBS)         # 短すぎ
        self.assertEqual(last_print(out)['dropped_short'], 1)
        self.assertEqual(subtitles(out), [GUIDE_REC, '-'])
        self.assertEqual(last_print(out)['guide'], 0)

    def test_a_silent_recording_clears_the_guide_too(self):
        out = self.turn(ms=1500, level=0, subs=self.SUBS)
        self.assertEqual(last_print(out)['dropped_silent'], 1)
        self.assertEqual(subtitles(out), [GUIDE_REC, '-'])

    # ---- 字幕の無い返答 ---------------------------------------------------
    def test_a_reply_without_subtitles_clears_the_guide_when_it_starts(self):
        # 「考えています…」のまま鳴らし続けない。
        out = self.turn()
        subs = subtitles(out)
        self.assertEqual(subs, [GUIDE_REC, GUIDE_THINK, '-'])

    # ---- 文面 ------------------------------------------------------------
    def test_the_wording_can_be_replaced(self):
        # 将来 settings から変える余地 (いまは既定のまま使う)。
        header = Path(IDF, 'main', 'stackee_talksm.h').read_text()
        self.assertIn('stackee_talk_set_guides', header)
        self.assertIn('#define STACKEE_TALK_GUIDE_RECORDING', header)
        self.assertIn('#define STACKEE_TALK_GUIDE_THINKING', header)


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
        self.assertEqual(reply_subs(out),
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
        self.assertEqual(reply_subs(out)[0], 'タブ\tは本文に入らない')

    def test_other_escapes_survive(self):
        out = self.turn(done_body(r'0\t\"かぎ\" \\ と \/\n'))
        self.assertEqual(reply_subs(out)[0], '"かぎ" \\ と /')

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
        # ★ 字幕は 1 つも出ない。出るのは「案内を消した」の 1 回だけ
        #   (鳴らしているのに「考えています…」のままにしない)。
        self.assertEqual(reply_subs(out), ['-'])
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
        self.assertEqual(reply_subs(out)[0], 'つぎ')

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
        self.assertEqual(reply_subs(out)[0], 'あ' * 15)

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
        self.assertEqual(reply_subs(out),
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
        self.assertEqual(len(reply_subs(out)), 4)

    def test_pages_change_at_the_start_times(self):
        out = self.happy()
        lines = out.splitlines()
        play_at = [i for i, s in enumerate(lines) if s.startswith('PLAY ')][0]
        # ★ 案内 (録音中 / 考えています…) は鳴らす前に出るので数えない。
        shown = [i for i, s in enumerate(lines)
                 if s.startswith('SUB ') and
                 s[4:] not in (GUIDE_REC, GUIDE_THINK)]
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
        self.assertEqual(reply_subs(out), [r'よい', r'よい\nもうひとつ', '-'])

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
        for text in reply_subs(out):
            if text == '-':
                continue
            self.assertLessEqual(len(text.split(r'\n')), 3, text)

    def test_a_missing_subtitle_file_does_not_stop_the_reply(self):
        out = self.happy(status=404)
        self.assertIn('PLAY 48000', out)
        self.assertEqual(last_print(out)['state'], 'idle')
        self.assertEqual(last_print(out)['error'], '')
        # ★ 字幕は 1 つも出ない。出るのは「案内を消した」の 1 回だけ
        #   (鳴らしているのに「考えています…」のままにしない)。
        self.assertEqual(reply_subs(out), ['-'])

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
        # ★ 字幕は 1 つも出ない。出るのは「案内を消した」の 1 回だけ。
        self.assertEqual(reply_subs(out), ['-'])

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


def look_info(text):
    """lookprint の行 (最後のもの) を辞書にする。"""
    rows = re.findall(r'^LOOKINFO (.*)$', text, re.M)
    assert rows, text
    out = {}
    for part in rows[-1].split(' '):
        k, _, v = part.partition('=')
        out[k] = int(v) if re.fullmatch(r'-?\d+', v) else v
    return out


def ctypes(text):
    return re.findall(r'^CTYPE (\S+) (\S+) (\d+)$', text, re.M)


LOOK_ACCEPT = '{"id":"img1","status_url":"/jobs/img1"}'
# ★ サーバとの取り決め: done の形は /talk と完全に同じ (transcript は空か無し)。
LOOK_DONE = ('{"state":"done","reply":"りんごが見えるのだ","transcript":"",'
             '"audio_url":"/jobs/img1/audio","sample_rate":16000,"channels":1,'
             '"sample_width":2,"subtitles":"0\\tりんごが見えるのだ\\n"}')


class LookPathTest(unittest.TestCase):
    """送り先は STACKEE_TALK_URL の末尾 /talk を /look にしたもの。"""

    def test_replacements(self):
        cases = {
            '/talk': '/look',
            '/api/talk': '/api/look',
            '/stackee/v1/talk': '/stackee/v1/look',
            '/talkx': '-',
            '/xtalk': '-',
            '/talk/': '-',
            '/talk?x=1': '-',
            '/': '-',
            'talk': '-',
            '//talk': '-',
        }
        script = ''.join('lookpath %s\n' % k for k in cases)
        got = re.findall(r'^LOOKPATH (.*)$', run(script), re.M)
        self.assertEqual(got, list(cases.values()))


class LookTest(unittest.TestCase):
    """画像を見せる (POST /look)。受理より後ろは会話と同じ道を歩く。"""

    HAPPY = """
ackms 300
respdelay 20
resp 202 %s
resp 200 {"state":"processing"}
resp 200 %s
resp 200 PCM:16000
reserve
look 12345 %%d
t 8000
print
lookprint
""" % (LOOK_ACCEPT, LOOK_DONE)

    def test_the_image_goes_to_look_as_jpeg_and_the_rest_is_the_talk_path(self):
        out = run(self.HAPPY % 1)
        self.assertIn('RESERVE 0', out)
        self.assertIn('LOOK 1', out)
        self.assertNotIn('REC begin', out)          # 録音の道は通らない
        calls = http_calls(out)
        self.assertEqual([c[0] for c in calls], ['POST', 'GET', 'GET', 'GET'])
        self.assertEqual([c[1] for c in calls],
                         ['/look', '/jobs/img1?wait=25', '/jobs/img1?wait=25',
                          '/jobs/img1/audio'])
        # 本体は JPEG そのもの (WAV ヘッダを付けない)。長さもそのまま。
        self.assertEqual(int(calls[0][2]), 12345)
        ct = ctypes(out)
        self.assertEqual(len(ct), 1)                # POST のときだけ
        self.assertEqual(ct[0][0], 'image/jpeg')
        self.assertEqual(ct[0][1], 'ffd8')
        # 受理のあとは会話と 1 手も違わない。
        self.assertEqual(
            state_names(out),
            ['idle', 'upload', 'poll_wait', 'poll', 'poll_wait', 'poll',
             'audio', 'play_wait', 'playing', 'idle'])
        self.assertIn('ACK 300', out)
        self.assertIn('PLAY 16000', out)
        self.assertIn('りんごが見えるのだ', subtitles(out))   # 字幕も同じ
        self.assertIn('SHOW りんごが見えるのだ', out)
        info = look_info(out)
        self.assertEqual(info['look'], 1)
        self.assertEqual(info['play'], 1)
        self.assertEqual(info['job'], '/jobs/img1')
        self.assertEqual(info['reply_len'], len('りんごが見えるのだ'.encode()))
        self.assertEqual(info['audio_bytes'], 32000)
        self.assertEqual(info['bytes'], 12345)
        self.assertEqual((info['looks'], info['done'], info['unplayed']), (1, 1, 0))
        self.assertEqual(info['reserved'], 0)
        self.assertEqual(info['busy'], 0)
        p = last_print(out)
        self.assertEqual(p['state'], 'idle')
        self.assertEqual(p['alloc'], p['release'])   # 写した JPEG は返してある
        self.assertIn('"look":1,"played":1', out)

    def test_play_zero_stops_just_before_playing_and_makes_no_sound(self):
        out = run(self.HAPPY % 0)
        self.assertNotIn('ACK ', out.replace('ACK skip', ''))
        self.assertNotIn('ACK skip', out)           # 一次回答を頼みもしない
        self.assertNotRegex(out, r'(?m)^PLAY ')
        # PCM は取りに行って受け取っている (鳴らす直前まで全部の段を通る)。
        self.assertEqual(http_calls(out)[-1][1], '/jobs/img1/audio')
        self.assertEqual(state_names(out)[-3:], ['audio', 'play_wait', 'idle'])
        info = look_info(out)
        self.assertEqual(info['play'], 0)
        self.assertEqual(info['audio_bytes'], 32000)
        self.assertEqual(info['reply_len'], len('りんごが見えるのだ'.encode()))
        self.assertEqual((info['looks'], info['done'], info['unplayed']), (1, 1, 1))
        self.assertGreater(info['complete_ms'], 0)
        self.assertGreaterEqual(info['complete_ms'], info['audio_ready_ms'])
        self.assertEqual(info['error'], '-')
        p = last_print(out)
        self.assertEqual(p['alloc'], p['release'])
        self.assertIn('"look":1,"played":0', out)

    def test_the_talk_key_is_ignored_while_reserved_for_the_camera(self):
        out = run("""
mic 40
reserve
press
t 500
release
t 100
print
lookprint
""")
        self.assertIn('RESERVE 0', out)
        self.assertNotIn('REC begin', out)
        self.assertEqual(state_names(out), ['idle'])
        self.assertEqual(look_info(out)['busy'], 1)

    def test_the_talk_key_is_ignored_during_a_look_turn(self):
        out = run("""
ackms 0
respdelay 100000
resp 202 %s
mic 40
reserve
look 2000 0
t 10
press
t 500
release
t 100
""" % LOOK_ACCEPT)
        self.assertIn('LOOK 1', out)
        self.assertNotIn('REC begin', out)
        self.assertEqual(state_names(out), ['idle', 'upload'])

    def test_a_key_held_through_the_look_does_not_start_recording_after_it(self):
        """撮影中から押しっぱなしのキーは、画像の往復が終わっても録音を始めない。"""
        out = run("""
ackms 0
respdelay 10
resp 202 %s
resp 200 %s
resp 200 PCM:1600
mic 40
reserve
press
t 5
look 2000 0
t 3000
print
""" % (LOOK_ACCEPT, LOOK_DONE))
        self.assertNotIn('REC begin', out)
        self.assertEqual(last_print(out)['state'], 'idle')

    def test_the_camera_is_refused_while_talking(self):
        out = run("""
mic 40
press
t 100
reserve
""")
        self.assertIn('RESERVE 1', out)

    def test_the_camera_is_refused_while_the_talk_key_is_down(self):
        out = run("press\nreserve\n")
        self.assertIn('RESERVE 1', out)

    def test_the_camera_is_refused_while_a_reply_is_waiting(self):
        out = run("""
ackms 0
respdelay 100000
resp 202 %s
inject 16000
t 10
reserve
""" % ACCEPT_BODY)
        self.assertIn('RESERVE 1', out)

    def test_a_second_reserve_is_refused(self):
        out = run("reserve\nreserve\n")
        self.assertEqual(re.findall(r'^RESERVE (\d)$', out, re.M), ['0', '1'])

    def test_inject_is_refused_while_reserved_and_allowed_after_release(self):
        out = run("""
reserve
inject 16000
unreserve
inject 16000
""")
        self.assertEqual(re.findall(r'^INJECT (\d)$', out, re.M), ['0', '1'])

    def test_no_wifi_refuses_before_the_capture(self):
        out = run("net 0\nreserve\nt 1\nlookprint\n")
        self.assertIn('RESERVE 2', out)
        self.assertIn('SHOW 会話エラー: Wi-Fi 未接続です', out)
        info = look_info(out)
        self.assertEqual(info['reserved'], 0)
        self.assertEqual(info['error'], 'Wi-Fi')    # 空白で切れる (中身は上で見た)

    def test_a_server_busy_409_is_shown_and_returns_to_idle(self):
        out = run("""
ackms 0
respdelay 10
resp 409 {"error":"busy"}
reserve
look 3000 0
t 200
print
lookprint
""")
        self.assertIn('SHOW 会話エラー: サーバーが処理中です (HTTP 409)', out)
        p = last_print(out)
        self.assertEqual(p['state'], 'idle')
        self.assertEqual(p['alloc'], p['release'])
        info = look_info(out)
        self.assertEqual((info['looks'], info['done']), (1, 0))
        self.assertEqual(info['busy'], 0)

    def test_a_timeout_is_shown_and_returns_to_idle(self):
        out = run("""
ackms 0
respdelay 5
resp 202 %s
sticky 200 {"state":"processing"}
reserve
look 3000 0
t 395000
print
""" % LOOK_ACCEPT)
        p = last_print(out)
        self.assertEqual(p['state'], 'idle')
        self.assertIn('タイムアウト', p['error'])
        self.assertEqual(p['alloc'], p['release'])

    def test_an_image_over_512_kib_is_not_sent(self):
        out = run("reserve\nlook %d 0\nt 10\nprint\nlookprint\n" % (512 * 1024 + 1))
        self.assertIn('LOOK 0', out)
        self.assertEqual(http_calls(out), [])
        self.assertIn('大きすぎ', last_print(out)['error'])
        self.assertEqual(look_info(out)['reserved'], 0)

    def test_exactly_512_kib_is_sent(self):
        out = run("respdelay 100000\nreserve\nlook %d 0\nt 10\n" % (512 * 1024))
        self.assertIn('LOOK 1', out)
        self.assertEqual(int(http_calls(out)[0][2]), 512 * 1024)

    def test_something_that_is_not_a_jpeg_is_not_sent(self):
        out = run("reserve\nlook 1 0\nt 10\nprint\n")
        self.assertIn('LOOK 0', out)
        self.assertEqual(http_calls(out), [])

    def test_the_image_body_is_a_copy(self):
        """呼び手の入れ物は look の直後に壊して解放する。送る本体は写しなので
        中身 (和) が変わらず、ASan も何も言わない。"""
        out = run("respdelay 50\nreserve\nlook 5000 0\nt 10\n")
        expect = sum(((i * 7) & 0xFF) for i in range(5000))
        expect += (0xFF + 0xD8 + 0xFF + 0xD9) - sum(
            ((i * 7) & 0xFF) for i in (0, 1, 4998, 4999))
        self.assertEqual(int(ctypes(out)[0][2]), expect)

    def test_talk_still_posts_wav_to_talk_after_a_look(self):
        out = run("""
ackms 0
respdelay 5
resp 202 %s
resp 200 %s
resp 200 PCM:1600
resp 202 %s
resp 200 %s
resp 200 PCM:1600
reserve
look 2000 0
t 3000
mic 40
press
t 400
release
t 3000
print
lookprint
""" % (LOOK_ACCEPT, LOOK_DONE, ACCEPT_BODY, DONE_BODY))
        posts = [c for c in http_calls(out) if c[0] == 'POST']
        self.assertEqual([c[1] for c in posts], ['/look', '/talk'])
        self.assertEqual([c[0] for c in ctypes(out)], ['image/jpeg', 'audio/wav'])
        self.assertEqual(look_info(out)['look'], 0)     # 直近は会話
        self.assertIn('PLAY 1600', out)
        p = last_print(out)
        self.assertEqual(p['alloc'], p['release'])

    def test_a_url_that_does_not_end_in_talk_cannot_look(self):
        out = run("init /chat\nreserve\nt 1\nlook 2000 0\nt 10\n")
        self.assertIn('RESERVE 2', out)
        self.assertIn('SHOW 会話エラー: STACKEE_TALK_URL が /talk で終わっていません', out)
        self.assertIn('LOOK 0', out)
        self.assertEqual(http_calls(out), [])

    def test_an_unset_url_cannot_look(self):
        out = run("init \nreserve\nt 1\n")
        self.assertIn('RESERVE 2', out)
        self.assertIn('SHOW 会話エラー: STACKEE_TALK_URL が未設定です', out)

    def test_a_nested_talk_path_looks_next_to_it(self):
        out = run("init /api/talk\nrespdelay 100000\nreserve\nlook 2000 0\nt 10\n")
        self.assertEqual(http_calls(out)[0][1], '/api/look')


# ---------------------------------------------------------------------------
# stackee 独自キー CSTM_0〜CSTM_9 (POST /key + 受け箱、2026-09-27)
# ---------------------------------------------------------------------------
def cstm_info(text):
    """cstmprint の行 (最後のもの) を辞書にする。error= だけは空白を含む。"""
    rows = re.findall(r'^CSTMINFO (.*)$', text, re.M)
    assert rows, text
    head, _, error = rows[-1].partition(' error=')
    out = {'error': error}
    for part in head.split(' '):
        k, _, v = part.partition('=')
        out[k] = int(v) if re.fullmatch(r'-?\d+', v) else v
    return out


def say_log(text):
    out = []
    for row in re.findall(r'^SAY (\d+) (.*)$', text, re.M):
        item = {}
        for part in row[1].split(' '):
            k, _, v = part.partition('=')
            item[k] = int(v)
        out.append(item)
    return out


def band(text, cols=15, lines=3):
    """帯に出る形 (15 字ずつ、最大 3 行、`\\n` 区切り) を Python で作る。"""
    rows = [text[i:i + cols] for i in range(0, len(text), cols)][:lines]
    return r'\n'.join(rows)


def bodies(text):
    return re.findall(r'^BODY (.*)$', text, re.M)


SEQ7 = '{"state":"empty","seq":7}'
IGNORED = '{"state":"ignored"}'
PROMPT_ACCEPT = '{"id":"p1","status_url":"/jobs/p1","mode":"prompt"}'
PROMPT_DONE = ('{"state":"done","reply":"ボタンの返事なのだ","audio_url":'
               '"/jobs/p1/audio","sample_rate":16000,"channels":1,'
               '"sample_width":2,"subtitles":"0\\tボタンの返事なのだ\\n"}')
CMD_ACCEPT = '{"id":"c1","status_url":"/jobs/c1","mode":"command"}'
FMT = '"sample_rate":16000,"channels":1,"sample_width":2'
SAY8 = ('{"state":"say","seq":8,"reply":"はじめるのだ","audio_url":'
        '"/inbox/8/audio","audio_bytes":16000,%s,'
        '"subtitles":"0\\tはじめるのだ\\n"}' % FMT)
SAY9_TEXT = ('{"state":"say","seq":9,"reply":"字幕だけなのだ",%s}' % FMT)
EMPTY9 = '{"state":"empty","seq":9,"job_state":"processing"}'
DONE9 = '{"state":"empty","seq":9,"job_state":"done"}'


class CstmPathTest(unittest.TestCase):
    """送り先は /look と同じ規則 (末尾の /talk を置き換える)。"""

    def test_siblings(self):
        cases = [('/talk key', '/key'), ('/talk inbox', '/inbox'),
                 ('/api/talk key', '/api/key'), ('/api/talk inbox', '/api/inbox'),
                 ('/talk look', '/look'), ('/chat key', '-'), ('/talkx key', '-'),
                 ('/talk/ inbox', '-')]
        out = run(''.join('sibling %s\n' % c for c, _ in cases))
        self.assertEqual(re.findall(r'^SIBLING (.*)$', out, re.M),
                         [want for _, want in cases])

    def test_band_wrapping(self):
        out = run('wrap 15 3 会話エラー: サーバーが処理中です (HTTP 409)\n'
                  'wrap 15 3 CSTM_3 未設定\n'
                  'wrap 3 2 あいうえおかきくけこ\n'
                  'wrap 15 3 \n')
        got = re.findall(r'^WRAP (\d+) (.*)$', out, re.M)
        self.assertEqual(got[0], ('2', band('会話エラー: サーバーが処理中です (HTTP 409)')))
        self.assertEqual(got[1], ('1', 'CSTM_3 未設定'))
        self.assertEqual(got[2], ('2', r'あいう\nえおか'))     # 入らない残りは捨てる
        self.assertEqual(got[3], ('0', '-'))


class CstmTest(unittest.TestCase):
    """CSTM_n を押す → GET /inbox (seq) → POST /key → 3 方式。"""

    def test_ignored_is_silent_and_says_unset_briefly(self):
        out = run("""
respdelay 10
resp 200 %s
resp 200 %s
cstm 3 1
t 200
cstmprint
t 3000
cstmprint
""" % (SEQ7, IGNORED))
        self.assertIn('CSTM 1', out)
        calls = http_calls(out)
        self.assertEqual([(c[0], c[1]) for c in calls],
                         [('GET', '/inbox'), ('POST', '/key')])
        self.assertEqual(bodies(out), ['{"key":"CSTM_3"}'])
        self.assertEqual([c[0] for c in ctypes(out)], ['application/json'])
        self.assertIn('SHOW CSTM_3 未設定', out)
        self.assertNotRegex(out, r'(?m)^(ACK|PLAY) ')      # 音なし
        # 帯: 考え中 → 「CSTM_3 未設定」を短く → 消える。
        subs = subtitles(out)
        self.assertIn('CSTM_3 未設定', subs)
        self.assertEqual(subs[-1], '-')
        rows = re.findall(r'^CSTMINFO (.*)$', out, re.M)
        self.assertIn('notice=1', rows[0])
        info = cstm_info(out)
        self.assertEqual((info['final'], info['says'], info['ignored']),
                         ('ignored', 0, 1))
        self.assertEqual((info['seq'], info['seqvalid'], info['reused']), (7, 1, 0))
        self.assertEqual((info['active'], info['talkbusy'], info['notice']), (0, 0, 0))
        self.assertGreater(info['seq_ms'], 0)
        self.assertGreaterEqual(info['key_ms'], info['seq_ms'])
        self.assertEqual(state_names(out), ['idle', 'inbox_seq', 'key', 'idle'])

    def test_a_409_says_the_server_is_busy(self):
        out = run("""
respdelay 10
resp 200 %s
resp 409 {"error":"busy"}
cstm 0 1
t 200
cstmprint
""" % SEQ7)
        self.assertIn('SHOW 会話エラー: サーバーが処理中です (HTTP 409)', out)
        info = cstm_info(out)
        self.assertEqual((info['final'], info['errors']), ('error', 1))
        self.assertIn('サーバーが処理中です', info['error'])
        # 帯にも短く出る (15 字で折り返す)。
        self.assertIn(band('会話エラー: サーバーが処理中です (HTTP 409)'),
                      subtitles(out))

    def test_prompt_is_the_same_second_half_as_look(self):
        script = """
ackms 300
respdelay 20
resp 200 %s
resp 202 %s
resp 200 {"state":"processing"}
resp 200 %s
resp 200 PCM:16000
cstm 2 %%d
t 8000
print
cstmprint
""" % (SEQ7, PROMPT_ACCEPT, PROMPT_DONE)
        out = run(script % 1)
        calls = http_calls(out)
        self.assertEqual([(c[0], c[1]) for c in calls],
                         [('GET', '/inbox'), ('POST', '/key'),
                          ('GET', '/jobs/p1?wait=25'), ('GET', '/jobs/p1?wait=25'),
                          ('GET', '/jobs/p1/audio')])
        self.assertEqual(
            state_names(out),
            ['idle', 'inbox_seq', 'key', 'poll_wait', 'poll', 'poll_wait', 'poll',
             'audio', 'play_wait', 'playing', 'idle'])
        self.assertIn('ACK 300', out)            # /look と同じく一次回答
        self.assertIn('PLAY 16000', out)
        self.assertIn('ボタンの返事なのだ', subtitles(out))
        info = cstm_info(out)
        self.assertEqual((info['mode'], info['final'], info['job']),
                         ('prompt', 'done', 'p1'))
        p = last_print(out)
        self.assertEqual(p['state'], 'idle')
        self.assertEqual(p['alloc'], p['release'])
        # play=0: 一次回答も返答も鳴らさず、PCM は受け取っている。
        out = run(script % 0)
        self.assertNotRegex(out, r'(?m)^(ACK|PLAY) ')
        self.assertEqual(http_calls(out)[-1][1], '/jobs/p1/audio')
        self.assertEqual(state_names(out)[-3:], ['audio', 'play_wait', 'idle'])
        self.assertEqual(cstm_info(out)['final'], 'done')
        self.assertIn('"look":0,"played":0', out)

    COMMAND = """
respdelay 10
resp 200 %s
resp 202 %s
resp 200 %s
resp 200 PCM:8000
resp 200 %s
resp 200 %s
resp 200 %s
cstm 5 %%d
t 20000
print
cstmprint
saylog
""" % (SEQ7, CMD_ACCEPT, SAY8, SAY9_TEXT, EMPTY9, DONE9)

    def test_command_plays_says_then_ends_on_done(self):
        out = run(self.COMMAND % 1)
        calls = [(c[0], c[1]) for c in http_calls(out)]
        self.assertEqual(calls, [
            ('GET', '/inbox'), ('POST', '/key'),
            ('GET', '/inbox?after=7&wait=25&job=c1'),
            ('GET', '/inbox/8/audio'),
            ('GET', '/inbox?after=8&wait=25&job=c1'),
            ('GET', '/inbox?after=9&wait=25&job=c1'),
            ('GET', '/inbox?after=9&wait=25&job=c1')])
        self.assertNotRegex(out, r'(?m)^ACK ')      # コマンドは一次回答なし
        self.assertEqual(re.findall(r'(?m)^PLAY (\d+)$', out), ['8000'])
        subs = reply_subs(out)
        self.assertIn('はじめるのだ', subs)           # 音つき発話の字幕
        self.assertIn('字幕だけなのだ', subs)         # 音なし発話は字幕だけ
        self.assertIn(GUIDE_THINK, subtitles(out))   # 発話の合間は考え中
        self.assertEqual(subtitles(out)[-1], '-')
        info = cstm_info(out)
        self.assertEqual((info['mode'], info['final'], info['says'], info['job']),
                         ('command', 'done', 2, 'c1'))
        self.assertEqual((info['seq'], info['polls'], info['active']), (9, 4, 0))
        self.assertEqual(info['jobstate'], 'done')
        self.assertGreater(info['first_say_ms'], info['key_ms'])
        self.assertGreaterEqual(info['end_ms'], info['first_say_ms'])
        log = say_log(out)
        self.assertEqual(len(log), 2)
        self.assertEqual((log[0]['seq'], log[0]['audio'], log[0]['played'],
                          log[0]['audio_bytes'], log[0]['got']),
                         (8, 1, 1, 16000, 16000))
        self.assertGreater(log[0]['sub_bytes'], 0)
        self.assertEqual((log[1]['seq'], log[1]['audio'], log[1]['played'],
                          log[1]['got']), (9, 0, 0, 0))
        self.assertGreaterEqual(log[1]['pages'], 1)
        p = last_print(out)
        self.assertEqual(p['state'], 'idle')
        self.assertIn('"final":"done"', out)

    def test_command_with_play_zero_fetches_but_never_plays(self):
        out = run(self.COMMAND % 0)
        self.assertNotRegex(out, r'(?m)^(ACK|PLAY) ')
        self.assertIn(('GET', '/inbox/8/audio'),
                      [(c[0], c[1]) for c in http_calls(out)])
        log = say_log(out)
        self.assertEqual((log[0]['got'], log[0]['played']), (16000, 0))
        self.assertEqual(cstm_info(out)['final'], 'done')

    def test_a_text_only_say_without_subtitles_is_wrapped_from_the_reply(self):
        long_reply = 'あ' * 20 + 'い' * 20
        say = ('{"state":"say","seq":8,"reply":"%s",%s}' % (long_reply, FMT))
        out = run("""
respdelay 10
resp 200 %s
resp 202 %s
resp 200 %s
resp 200 %s
cstm 1 1
t 20000
cstmprint
saylog
""" % (SEQ7, CMD_ACCEPT, say, DONE9.replace('"seq":9', '"seq":8')))
        self.assertNotRegex(out, r'(?m)^PLAY ')
        # 3 行ぶん (帯 1 枚) は同じ時刻に出るので、まとめて 1 回で出る。
        self.assertIn(band(long_reply), reply_subs(out))
        self.assertEqual(say_log(out)[0]['pages'], 3)
        self.assertEqual(cstm_info(out)['final'], 'done')

    def test_command_error_is_shown_like_a_talk_error(self):
        out = run("""
respdelay 10
resp 200 %s
resp 202 %s
resp 200 {"state":"empty","seq":7,"job_state":"error","error":"コマンドが落ちたのだ"}
cstm 4 1
t 3000
cstmprint
""" % (SEQ7, CMD_ACCEPT))
        self.assertIn('SHOW 会話エラー: コマンドが落ちたのだ', out)
        info = cstm_info(out)
        self.assertEqual((info['final'], info['error']), ('error', 'コマンドが落ちたのだ'))
        self.assertIn(band('会話エラー: コマンドが落ちたのだ'), subtitles(out))
        self.assertNotRegex(out, r'(?m)^(ACK|PLAY) ')

    def test_an_unknown_job_state_ends_with_an_error(self):
        out = run("""
respdelay 10
resp 200 %s
resp 202 %s
resp 200 {"state":"empty","seq":7,"job_state":"lost"}
cstm 4 1
t 3000
cstmprint
""" % (SEQ7, CMD_ACCEPT))
        self.assertEqual(cstm_info(out)['final'], 'error')
        self.assertIn('lost', cstm_info(out)['error'])

    def test_an_old_relay_without_inbox_says_so_and_sends_nothing(self):
        out = run("""
respdelay 10
resp 404 not found
cstm 3 1
t 200
cstmprint
""")
        self.assertEqual([(c[0], c[1]) for c in http_calls(out)], [('GET', '/inbox')])
        self.assertIn('SHOW 会話エラー: 中継が /inbox に対応していません (HTTP 404)', out)
        info = cstm_info(out)
        self.assertEqual((info['final'], info['seqvalid']), ('error', 0))

    def test_a_404_while_polling_the_inbox_ends_the_command(self):
        out = run("""
respdelay 10
resp 200 %s
resp 202 %s
resp 404 nope
cstm 3 1
t 2000
cstmprint
""" % (SEQ7, CMD_ACCEPT))
        self.assertIn('HTTP 404', cstm_info(out)['error'])
        self.assertEqual(cstm_info(out)['seqvalid'], 0)

    def test_a_broken_connection_while_polling_ends_with_an_error(self):
        out = run("""
respdelay 10
resp 200 %s
resp 202 %s
resperr
cstm 3 1
t 2000
print
cstmprint
""" % (SEQ7, CMD_ACCEPT))
        self.assertEqual(cstm_info(out)['error'], '通信に失敗しました')
        self.assertEqual(last_print(out)['state'], 'idle')

    def test_a_say_already_seen_is_skipped(self):
        out = run("""
respdelay 10
resp 200 %s
resp 202 %s
resp 200 {"state":"say","seq":7,"reply":"古いのだ",%s}
resp 200 %s
cstm 3 1
t 5000
cstmprint
""" % (SEQ7, CMD_ACCEPT, FMT, DONE9.replace('"seq":9', '"seq":7')))
        self.assertNotIn('古いのだ', '\n'.join(subtitles(out)))
        self.assertEqual(cstm_info(out)['says'], 0)
        self.assertEqual(cstm_info(out)['final'], 'done')

    def test_a_bad_audio_format_in_a_say_is_an_error(self):
        say = SAY8.replace('"sample_rate":16000', '"sample_rate":8000')
        out = run("""
respdelay 10
resp 200 %s
resp 202 %s
resp 200 %s
cstm 3 1
t 2000
cstmprint
""" % (SEQ7, CMD_ACCEPT, say))
        self.assertEqual(cstm_info(out)['error'], '返答の音声形式が違います')
        self.assertNotRegex(out, r'(?m)^PLAY ')

    def test_the_whole_command_gives_up_after_660_seconds(self):
        out = run("""
respdelay 5
resp 200 %s
resp 202 %s
sticky 200 {"state":"empty","seq":7,"job_state":"processing"}
cstm 3 1
t 30000
cstmprint
t 640000
print
cstmprint
""" % (SEQ7, CMD_ACCEPT))
        rows = re.findall(r'^CSTMINFO (.*)$', out, re.M)
        self.assertIn('active=1', rows[0])
        self.assertIn('uses_audio=0', rows[0])     # 待っている間は USB マイクを止めない
        self.assertIn('talkbusy=1', rows[0])
        info = cstm_info(out)
        self.assertEqual(info['final'], 'error')
        self.assertIn('タイムアウト', info['error'])
        self.assertEqual(last_print(out)['state'], 'idle')

    def test_a_long_poll_that_takes_seconds_is_reissued_without_a_gap(self):
        out = run("""
respdelay 10
resp 200 %s
resp 202 %s
respdelay 3000
resp 200 %s
resp 200 %s
cstm 3 1
t 10000
print
""" % (SEQ7, CMD_ACCEPT, EMPTY9, DONE9))
        # 2 本目は 1 本目が返った直後に撃っている (1 秒あけない)。
        stamps = [int(ms) for s, ms in states(out) if s == 'inbox']
        self.assertEqual(len(stamps), 2)
        self.assertLess(stamps[1] - stamps[0], 3000 + 50)

    def test_the_seq_is_reused_and_refreshed_after_the_ttl(self):
        out = run("""
respdelay 10
resp 200 %s
resp 200 %s
resp 200 %s
resp 200 {"state":"empty","seq":20}
resp 200 %s
cstm 1 1
t 3000
cstm 2 1
t 3000
cstmprint
t 300000
cstm 3 1
t 3000
cstmprint
""" % (SEQ7, IGNORED, IGNORED, IGNORED))
        calls = [(c[0], c[1]) for c in http_calls(out)]
        self.assertEqual(calls, [('GET', '/inbox'), ('POST', '/key'),
                                 ('POST', '/key'),
                                 ('GET', '/inbox'), ('POST', '/key')])
        rows = re.findall(r'^CSTMINFO (.*)$', out, re.M)
        self.assertIn('reused=1', rows[0])
        self.assertIn('seq_ms=0', rows[0])
        self.assertIn('reused=0', rows[1])
        self.assertIn('seq=20', rows[1])

    def test_the_last_seen_seq_carries_over_to_the_next_command(self):
        out = run("""
respdelay 10
resp 200 %s
resp 202 %s
resp 200 {"state":"empty","seq":12,"job_state":"done"}
resp 202 {"id":"c2","status_url":"/jobs/c2","mode":"command"}
resp 200 {"state":"empty","seq":12,"job_state":"done"}
cstm 1 1
t 3000
cstm 1 1
t 3000
""" % (SEQ7, CMD_ACCEPT))
        self.assertIn(('GET', '/inbox?after=12&wait=25&job=c2'),
                      [(c[0], c[1]) for c in http_calls(out)])

    def test_bad_n_is_refused(self):
        out = run('cstm 10 1\ncstm -1 1\n')
        self.assertEqual(re.findall(r'^CSTM (\d)$', out, re.M), ['0', '0'])
        self.assertEqual(http_calls(out), [])

    def test_no_wifi_and_no_url_are_errors_without_sending(self):
        out = run('net 0\ncstm 3 1\nt 10\ncstmprint\n')
        self.assertIn('SHOW 会話エラー: Wi-Fi 未接続です', out)
        self.assertEqual(http_calls(out), [])
        self.assertEqual(cstm_info(out)['final'], 'error')
        out = run('init /chat\ncstm 3 1\nt 10\n')
        self.assertIn('SHOW 会話エラー: STACKEE_TALK_URL が /talk で終わっていません', out)
        self.assertEqual(http_calls(out), [])

    def test_unknown_mode_and_bad_id_are_errors(self):
        for accept, why in (
                ('{"id":"c1","status_url":"/jobs/c1","mode":"magic"}', 'mode'),
                ('{"id":"c 1","status_url":"/jobs/c1","mode":"command"}', 'id'),
                ('{"state":"done"}', None)):
            status = 200 if why is None else 202
            out = run("""
respdelay 10
resp 200 %s
resp %d %s
cstm 3 1
t 200
cstmprint
""" % (SEQ7, status, accept))
            info = cstm_info(out)
            self.assertEqual(info['final'], 'error', accept)
            if why:
                self.assertIn(why, info['error'])


class CstmExclusionTest(unittest.TestCase):
    """会話キー・カメラ・CSTM は同時に 1 つ。処理中の CSTM は黙って無視。"""

    HOLD = """
respdelay 100000
resp 200 %s
cstm 3 1
t 10
""" % SEQ7

    def test_a_cstm_while_talking_is_ignored_silently(self):
        out = run("mic 40\npress\nt 100\ncstm 3 1\nt 10\ncstmprint\n")
        self.assertIn('CSTM 0', out)
        info = cstm_info(out)
        self.assertEqual((info['busy'], info['count']), (1, 0))
        self.assertNotIn('/key', out)
        self.assertNotIn('未設定', out)

    def test_a_cstm_while_the_talk_key_is_down_is_ignored(self):
        self.assertIn('CSTM 0', run('press\ncstm 3 1\n'))

    def test_a_cstm_while_reserved_for_the_camera_is_ignored(self):
        self.assertIn('CSTM 0', run('reserve\ncstm 3 1\n'))

    def test_a_cstm_while_a_look_is_running_is_ignored(self):
        out = run("respdelay 100000\nreserve\nlook 2000 0\nt 10\ncstm 3 1\n")
        self.assertIn('CSTM 0', out)

    def test_a_cstm_during_a_cstm_is_ignored(self):
        out = run(self.HOLD + 'cstm 4 1\ncstmprint\n')
        self.assertEqual(re.findall(r'^CSTM (\d)$', out, re.M), ['1', '0'])
        info = cstm_info(out)
        self.assertEqual((info['n'], info['busy'], info['count']), (3, 1, 1))

    def test_the_talk_key_is_ignored_during_a_cstm(self):
        out = run('mic 40\n' + self.HOLD + 'press\nt 500\nrelease\nt 100\n')
        self.assertNotIn('REC begin', out)

    def test_inject_and_the_camera_are_refused_during_a_cstm(self):
        out = run(self.HOLD + 'inject 16000\nreserve\n')
        self.assertIn('INJECT 0', out)
        self.assertIn('RESERVE 1', out)

    def test_the_camera_is_refused_while_a_command_speaks(self):
        out = run("""
respdelay 10
resp 200 %s
resp 202 %s
resp 200 %s
resp 200 PCM:80000
cstm 5 1
t 1500
reserve
inject 16000
print
""" % (SEQ7, CMD_ACCEPT, SAY8))
        self.assertEqual(last_print(out)['state'], 'playing')
        self.assertIn('RESERVE 1', out)
        self.assertIn('INJECT 0', out)

    def test_a_key_held_through_a_cstm_does_not_start_recording_after_it(self):
        out = run("""
respdelay 10
resp 200 %s
resp 200 %s
mic 40
cstm 3 1
t 5
press
t 3000
print
""" % (SEQ7, IGNORED))
        self.assertNotIn('REC begin', out)
        self.assertEqual(last_print(out)['state'], 'idle')

    def test_talk_and_look_still_work_after_a_cstm(self):
        out = run("""
ackms 0
respdelay 5
resp 200 %s
resp 200 %s
resp 202 %s
resp 200 %s
resp 200 PCM:1600
resp 202 %s
resp 200 %s
resp 200 PCM:1600
cstm 3 1
t 3000
mic 40
press
t 400
release
t 3000
reserve
look 2000 0
t 3000
print
lookprint
""" % (SEQ7, IGNORED, ACCEPT_BODY, DONE_BODY, LOOK_ACCEPT, LOOK_DONE))
        posts = [c[1] for c in http_calls(out) if c[0] == 'POST']
        self.assertEqual(posts, ['/key', '/talk', '/look'])
        self.assertEqual([c[0] for c in ctypes(out)],
                         ['application/json', 'audio/wav', 'image/jpeg'])
        self.assertIn('PLAY 1600', out)
        info = look_info(out)
        self.assertEqual((info['look'], info['done']), (1, 1))
        p = last_print(out)
        self.assertEqual(p['alloc'], p['release'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
