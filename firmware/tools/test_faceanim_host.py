#!/usr/bin/env python3
"""顔の状態機械を Mac 上で走らせて、時刻の境目を 1 ms 単位で確かめる。

実機には触らない。見ているのは firmware/kmk/stackee_face.py から移した
タイミングが 1 つも変わっていないこと:

  ・起動から 2000 ms は awake、そのあと idle
  ・待機・考え中・発話中は 3000 ms でグループを選び直す
  ・聞き取り 250 ms / 考え中 700 ms / 発話 250 ms でフレームが進む
  ・聞き取り・考え中のフレームは順送り、発話だけ「前と違うものを選ぶ」
  ・打鍵のあと 1000 ms は待機中の表情切り替えを始めない (TYPING_PAUSE)
  ・撮影が終わってから 1500 ms は camera を保つ
  ・差分は 1 周 16 行 (240 行なら 15 周)

  python3 firmware/tools/test_faceanim_host.py
  python3 -m pytest firmware/tools/test_faceanim_host.py
"""
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

IDF = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(IDF / 'tools'))
import stackee_tree as tree                     # noqa: E402

MANIFEST = tree.asset('manifest.json')
if MANIFEST is None:
    raise unittest.SkipTest('素材の目録 (manifest.json) が無い')

_BINARY = None


def binary():
    global _BINARY
    if _BINARY is None:
        tmp = tempfile.mkdtemp()
        out = Path(tmp) / 'faceanim'
        cmd = ['cc', '-O1', '-std=gnu11', '-Wall', '-Werror',
               '-I', str(IDF / 'main'), '-o', str(out),
               str(IDF / 'hostbuild/faceanim_main.c'),
               str(IDF / 'main/stackee_faceanim.c')]
        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError('ホストビルドに失敗:\n' + build.stderr)
        _BINARY = out
    return _BINARY


def run(script):
    out = subprocess.run([str(binary()), str(MANIFEST)], input=script,
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise AssertionError('実行に失敗:\n' + out.stderr)
    rows = []
    cases = {}
    for line in out.stdout.splitlines():
        if line.startswith('case '):
            name, groups, frames = re.match(
                r'case (\S+) groups=(\d+) frames=(\S+)', line).groups()
            cases[name] = (int(groups), [int(n) for n in frames.split(',')])
            continue
        fields = dict(part.split('=', 1) for part in line.split())
        fields['t'] = int(fields['t'])
        for key in ('group', 'frame', 'face', 'cur', 'tgt'):
            fields[key] = int(fields[key])
        fields['skipped'] = int(fields['skipped'])
        fields['paints'] = int(fields['paints'])
        rows.append(fields)
    return cases, rows


def settle(start, count=20, step=1):
    """遷移 (16 行ずつ) を最後まで流すための tick の並び。"""
    return ''.join('t %d\n' % (start + i * step) for i in range(count))


class CasesTest(unittest.TestCase):
    def test_manifest_cases(self):
        cases, _ = run('t 0\n')
        # 本物の manifest.json の形 (顔 32 枚の内訳)。
        self.assertEqual(cases['awake'], (1, [2]))
        self.assertEqual(cases['idle'], (8, [1] * 8))
        self.assertEqual(cases['listening'], (1, [3]))
        self.assertEqual(cases['thinking'], (1, [2]))
        self.assertEqual(cases['speaking'], (4, [4, 4, 4, 4]))
        self.assertEqual(cases['camera'], (1, [1]))
        self.assertEqual(cases['microphone'], (1, [3]))


class AwakeTest(unittest.TestCase):
    def test_awake_lasts_exactly_2000ms(self):
        _, rows = run('t 0\nt 1\nt 1999\nt 2000\n')
        self.assertEqual([r['state'] for r in rows],
                         ['awake', 'awake', 'awake', 'idle'])

    def test_awake_never_changes_frame(self):
        # awake は「3 秒でグループ」にも「250 ms でフレーム」にも入らない。
        _, rows = run(''.join('t %d\n' % t for t in range(0, 2000, 100)))
        self.assertEqual({r['face'] for r in rows}, {rows[0]['face']})


class DiffPaintTest(unittest.TestCase):
    def test_16_rows_per_pass(self):
        _, rows = run('t 1999\n' + settle(2000, count=16))
        painted = [r for r in rows if r['rect'] != '-']
        # 240 行 / 16 行 = 15 周でちょうど終わる。
        self.assertEqual(len(painted), 15)
        self.assertEqual(painted[0]['rect'], '0,0,240,16')
        self.assertEqual(painted[-1]['rect'], '0,224,240,16')
        self.assertEqual(painted[-1]['tgt'], -1, '最後の周で遷移が終わる')
        self.assertEqual(rows[-1]['cur'], painted[-1]['face'])


class TypingPauseTest(unittest.TestCase):
    """打鍵の直後は待機中のまばたきを始めない。"""

    def _script(self, key_at, probe_at):
        # まず idle に落ち着かせる (awake -> idle の遷移も終わらせる)。
        script = 't 1999\n' + settle(2000, count=16)
        script += 'key %d\n' % key_at
        # グループの選び直し (3000 ms) が来る時刻まで飛ばす。
        script += 't %d\n' % probe_at
        return script

    def test_no_blink_while_typing(self):
        # group_at は 2000。3000 ms 後 = 5000 に選び直しが来るが、
        # 4500 に打鍵していれば 5000 (< 4500+1000) では始めない。
        _, rows = run(self._script(4500, 5000))
        last = rows[-1]
        self.assertEqual(last['rect'], '-')
        self.assertEqual(last['skipped'], 1)

    def test_blink_resumes_after_1000ms(self):
        # 4500 に打鍵 → 5500 (= 4500+1000) からは始める。
        _, rows = run(self._script(4500, 5500))
        last = rows[-1]
        self.assertNotEqual(last['rect'], '-')
        self.assertEqual(last['skipped'], 0)

    def test_transition_in_flight_is_not_paused(self):
        # 遷移の途中 (tgt >= 0) なら打鍵中でも塗り続ける。
        script = 't 1999\nt 2000\n'      # 遷移を始めるが終わらせない
        script += 'key 2001\nt 2002\n'
        _, rows = run(script)
        self.assertNotEqual(rows[-1]['rect'], '-')
        self.assertEqual(rows[-1]['skipped'], 0)


class ConversationTest(unittest.TestCase):
    def test_listening_advances_every_250ms(self):
        script = 'rec 1\nt 10\n' + settle(11, count=16)
        script += 't 259\n'         # 10 + 249
        script += 't 260\n'         # 10 + 250
        _, rows = run(script)
        self.assertEqual(rows[0]['state'], 'listening')
        at_249 = [r for r in rows if r['t'] == 259][0]
        at_250 = [r for r in rows if r['t'] == 260][0]
        self.assertEqual(at_249['face'], rows[0]['face'], '249 ms では変わらない')
        self.assertNotEqual(at_250['face'], rows[0]['face'], '250 ms で変わる')

    def test_thinking_advances_every_700ms(self):
        script = 'busy 1\nt 10\n' + settle(11, count=16)
        script += 't 709\nt 710\n'
        _, rows = run(script)
        self.assertEqual(rows[0]['state'], 'thinking')
        self.assertEqual([r for r in rows if r['t'] == 709][0]['face'],
                         rows[0]['face'])
        self.assertNotEqual([r for r in rows if r['t'] == 710][0]['face'],
                            rows[0]['face'])

    def _frames_at(self, flag, interval, count):
        """250 / 700 ms の境目ごとに 1 枚ずつ、フレーム番号を拾う。"""
        boundaries = [10 + interval * i for i in range(5)]
        script = '%s 1\n' % flag
        for t in boundaries:
            script += 't %d\n' % t + settle(t + 1, count=16)
        _, rows = run(script)
        frames, groups = [], []
        for t in boundaries:
            row = [r for r in rows if r['t'] == t][0]
            frames.append(row['frame'])
            groups.append(row['group'])
        self.assertEqual(len(set(groups)), 1, '3 秒以内はグループを変えない')
        return frames

    def test_pc_mic_uses_its_own_three_frames_and_exact_timing(self):
        # 案11: 1 -> 2 -> 3を550/550/650 ms。3秒を過ぎてもリセットしない。
        probes = [10, 559, 560, 1109, 1110, 1759, 1760,
                  2309, 2310, 2859, 2860, 3509, 3510]
        script = 'mic 1\n'
        for t in probes:
            script += 't %d\n' % t
        _, rows = run(script)
        self.assertTrue(all(r['state'] == 'microphone' for r in rows))
        self.assertEqual([r['face'] for r in rows],
                         [32, 32, 33, 33, 34, 34, 32, 32, 33, 33, 34, 34, 32])

    def test_mic_and_ai_recording_use_different_assets(self):
        _, mic = run('mic 1\nt 10\n')
        _, rec = run('rec 1\nt 10\n')
        self.assertEqual(mic[0]['face'], 32)
        self.assertIn(rec[0]['face'], (10, 11, 12))
        self.assertEqual(rec[0]['state'], 'listening')

    def test_mic_reentry_starts_at_first_frame(self):
        script = 'mic 1\nt 10\n' + settle(11, 16)
        script += 't 560\n' + settle(561, 16)
        script += 'mic 0\nt 2200\n' + settle(2201, 16)
        script += 'mic 1\nt 2300\n' + settle(2301, 16)
        _, rows = run(script)
        self.assertEqual(rows[-1]['state'], 'microphone')
        self.assertEqual(rows[-1]['cur'], 32)

    def test_releasing_mic_during_ai_wait_returns_to_thinking(self):
        script = 'busy 1\nmic 1\nt 10\n' + settle(11, 16)
        script += 'mic 0\nt 1000\n' + settle(1001, 16)
        _, rows = run(script)
        self.assertEqual(rows[-1]['state'], 'thinking')
        self.assertIn(rows[-1]['cur'], (13, 14))

    def test_recording_interrupts_pc_mic_and_pc_mic_resumes(self):
        script = 'mic 1\nt 10\n' + settle(11, 16)
        script += 'rec 1\nt 100\n' + settle(101, 16)
        script += 'rec 0\nt 200\n' + settle(201, 16)
        _, rows = run(script)
        recording = next(r for r in rows if r['t'] == 116)
        self.assertEqual(recording['state'], 'listening')
        self.assertIn(recording['cur'], (10, 11, 12))
        self.assertEqual(rows[-1]['state'], 'microphone')
        self.assertEqual(rows[-1]['cur'], 32)

    def test_releasing_the_mic_key_goes_back_to_idle(self):
        script = 'mic 1\nt 10\n' + settle(11, count=16)
        script += 'mic 0\nt 3000\n' + settle(3001, count=16)
        _, rows = run(script)
        self.assertEqual(rows[0]['state'], 'microphone')
        self.assertEqual(rows[-1]['state'], 'idle')

    def test_the_mic_key_sits_between_recording_and_thinking(self):
        """優先順位: speaking > 録音 > mic > 考え中 > カメラ > awake/idle。"""
        def state(flags, at=10):
            script = ''.join('%s 1\n' % f for f in flags) + 't %d\n' % at
            _, rows = run(script)
            return rows[-1]['state']

        self.assertEqual(state(['mic']), 'microphone')
        # 本体が自分で録っているならそちらが勝つ。
        self.assertEqual(state(['rec', 'mic']), 'listening')
        self.assertEqual(state(['speak', 'mic']), 'speaking')
        # 返答を待っている間に PC へ喋り始めたらマイクの受付サインに戻る。
        self.assertEqual(state(['busy', 'mic']), 'microphone')
        self.assertEqual(state(['busy']), 'thinking')
        # カメラより上。
        self.assertEqual(state(['cam', 'mic']), 'microphone')
        self.assertEqual(state(['cam']), 'camera')
        # 起動直後の awake より上 (2 秒以内でも聞き取り中)。
        self.assertEqual(state(['mic'], at=100), 'microphone')
        self.assertEqual(state([], at=100), 'awake')

    def test_speaking_frames_never_repeat(self):
        # ★ 発話中だけ **順送りではない**。stackee_face.py の
        #   `self._frame(now, sequential=state != 'speaking')` そのまま:
        #   発話は「前と違うものを 1 つ」、聞き取り・考え中は順送り。
        frames = self._frames_at('speak', 250, 4)
        for before, after in zip(frames, frames[1:]):
            self.assertNotEqual(after, before,
                                '発話中は同じ顔を続けない: %r' % (frames,))

    def test_listening_frames_are_sequential(self):
        frames = self._frames_at('rec', 250, 3)
        for before, after in zip(frames, frames[1:]):
            self.assertEqual(after, (before + 1) % 3,
                             '聞き取り中のフレームは順送り: %r' % (frames,))

    def test_thinking_frames_are_sequential(self):
        frames = self._frames_at('busy', 700, 2)
        for before, after in zip(frames, frames[1:]):
            self.assertEqual(after, (before + 1) % 2,
                             '考え中のフレームは順送り: %r' % (frames,))

    def test_priority_speaking_over_recording(self):
        _, rows = run('speak 1\nrec 1\nbusy 1\ncam 1\nt 10\n')
        self.assertEqual(rows[-1]['state'], 'speaking')

    def test_priority_recording_over_busy(self):
        _, rows = run('rec 1\nbusy 1\ncam 1\nt 10\n')
        self.assertEqual(rows[-1]['state'], 'listening')

    def test_group_repick_after_3000ms(self):
        # idle は 8 グループ。3000 ms ごとに選び直す。
        script = 't 1999\n' + settle(2000, count=16)
        script += 't 4999\nt 5000\n'
        _, rows = run(script)
        before = [r for r in rows if r['t'] == 4999][0]
        after = [r for r in rows if r['t'] == 5000][0]
        self.assertEqual(before['rect'], '-', '2999 ms ではまだ')
        self.assertNotEqual(after['rect'], '-', '3000 ms で選び直す')
        self.assertNotEqual(after['group'], before['group'])


class CameraTest(unittest.TestCase):
    def test_camera_is_held_1500ms_after_capture(self):
        script = 'cam 1\nt 100\n' + settle(101, count=16)
        script += 'cam 0\nt 3000\n'                 # 撮影おわり
        script += 't 4499\n'                        # 3000 + 1499
        script += 't 4500\n'                        # 3000 + 1500
        _, rows = run(script)
        self.assertEqual(rows[0]['state'], 'camera')
        self.assertEqual([r for r in rows if r['t'] == 3000][0]['state'], 'camera')
        self.assertEqual([r for r in rows if r['t'] == 4499][0]['state'], 'camera')
        self.assertEqual([r for r in rows if r['t'] == 4500][0]['state'], 'idle')


if __name__ == '__main__':
    unittest.main(verbosity=2)
