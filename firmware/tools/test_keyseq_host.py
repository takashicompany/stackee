#!/usr/bin/env python3
"""打鍵列テスト。QMK + 橋渡し層を Mac 上でビルドして走らせる。

    python3 firmware/tools/test_keyseq_host.py
    python3 -m pytest firmware/tools/test_keyseq_host.py

実機には触らない。確かめているのは「時刻つきの押下 / 解放の列を入れたら、
現行 CircuitPython + KMK 版と同じ HID レポートの列が出るか」だけ。
I2C も USB も入っていないので、ここで通っても実機で動く保証にはならない。

期待値の出どころは firmware/kmk/keymap.py と KMK の HoldTap の仕様
(kmk/modules/holdtap.py)。対応表は tools/keycodes.md。
"""
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
IDF = os.path.dirname(HERE)
QMK = os.path.join(IDF, 'third_party', 'qmk')

# QMK 本体のうちビルドするもの (main/CMakeLists.txt と同じ並び)。
QMK_SOURCES = [
    'quantum/quantum.c', 'quantum/keyboard.c', 'quantum/action.c',
    'quantum/action_tapping.c', 'quantum/action_util.c', 'quantum/action_layer.c',
    'quantum/keycode_config.c', 'quantum/keymap_common.c',
    'quantum/keymap_introspection.c', 'quantum/matrix_common.c',
    'quantum/eeconfig.c', 'quantum/dynamic_keymap.c', 'quantum/via.c',
    'quantum/bitwise.c', 'quantum/led.c', 'quantum/mousekey.c',
    'quantum/sync_timer.c',
    'quantum/process_keycode/process_quantum.c',
    'quantum/process_keycode/process_default_layer.c',
    'quantum/debounce/sym_defer_pk.c', 'quantum/send_string/send_string.c',
    'quantum/logging/debug.c', 'quantum/logging/print.c',
    'quantum/logging/sendchar.c',
    'quantum/nvm/eeprom/nvm_eeconfig.c', 'quantum/nvm/eeprom/nvm_dynamic_keymap.c',
    'quantum/nvm/eeprom/nvm_via.c',
    'tmk_core/protocol/host.c', 'tmk_core/protocol/report.c',
    'tmk_core/protocol/usb_device_state.c',
    'platforms/suspend.c',
]

PORT_SOURCES = [
    'main/qmk_port/qmk_port_timer.c', 'main/qmk_port/qmk_port_matrix.c',
    'main/qmk_port/qmk_port_host.c', 'main/qmk_port/qmk_port_eeprom.c',
    'main/qmk_port/qmk_port_stubs.c', 'main/qmk_port/qmk_port_init.c',
    'main/qmk_port/stackee_holdtap.c', 'main/stackee_report_queue.c',
    'hostbuild/keyseq_main.c',
]

# include の並びは main/CMakeLists.txt と同じ。qmk_port は QMK の**後ろ**。
# quantum/logging/print.h が include_next "_print.h" で後ろを探すため。
INCLUDES = [
    os.path.join(QMK, 'platforms'),
    os.path.join(QMK, 'quantum'),
    os.path.join(QMK, 'quantum', 'keymap_extras'),
    os.path.join(QMK, 'quantum', 'logging'),
    os.path.join(QMK, 'quantum', 'nvm'),
    os.path.join(QMK, 'quantum', 'nvm', 'eeprom'),
    os.path.join(QMK, 'quantum', 'process_keycode'),
    os.path.join(QMK, 'quantum', 'send_string'),
    os.path.join(QMK, 'quantum', 'sequencer'),
    os.path.join(QMK, 'tmk_core', 'protocol'),
    os.path.join(IDF, 'main'),
    os.path.join(IDF, 'main', 'qmk_port'),
]

_BINARY = None
_TMPDIR = None


def build_once():
    """1 回だけビルドして、できた実行ファイルの場所を返す。"""
    global _BINARY, _TMPDIR
    if _BINARY:
        return _BINARY
    _TMPDIR = tempfile.mkdtemp(prefix='stackee-keyseq-')
    binary = os.path.join(_TMPDIR, 'keyseq')
    cmd = ['cc', '-O1', '-std=gnu11', '-Wno-include-next-absolute-path',
           '-include', os.path.join(IDF, 'main', 'qmk_port', 'stackee_qmk_config.h'),
           '-DKEYMAP_C="%s"' % os.path.join(IDF, 'main', 'keymaps', 'default_keymap.c')]
    for path in INCLUDES:
        cmd += ['-I', path]
    cmd += [os.path.join(QMK, s) for s in QMK_SOURCES]
    cmd += [os.path.join(IDF, s) for s in PORT_SOURCES]
    cmd += ['-o', binary]
    done = subprocess.run(cmd, capture_output=True, text=True)
    if done.returncode != 0:
        raise AssertionError('ホストビルドに失敗:\n' + done.stderr[-4000:])
    _BINARY = binary
    return binary


def run(script):
    """台本を流して、出力を行のリストで返す。"""
    binary = build_once()
    done = subprocess.run([binary], input=script, capture_output=True, text=True)
    if done.returncode != 0:
        raise AssertionError('実行に失敗:\n' + done.stderr)
    return [line for line in done.stdout.splitlines() if line]


def after(lines, mark):
    """MARK <mark> の次から、次の MARK までを返す。"""
    try:
        start = lines.index('MARK ' + mark) + 1
    except ValueError:
        raise AssertionError('目印 %r が出力に無い: %r' % (mark, lines))
    out = []
    for line in lines[start:]:
        if line.startswith('MARK '):
            break
        out.append(line)
    return out


def kb(mods, *keys):
    """期待する KB 行を組み立てる。"""
    slots = list(keys) + [0] * (6 - len(keys))
    return 'KB %02X ' % mods + ' '.join('%02X' % k for k in slots)


NONE = kb(0x00)

# HID の使用番号 (firmware/kmk/.kmk_src の kmk/keys.py と同じ値)。
KC_Q, KC_W, KC_E, KC_R, KC_Z = 0x14, 0x1A, 0x08, 0x15, 0x1D
KC_ESC, KC_TAB, KC_2 = 0x29, 0x2B, 0x1F
KC_LNG1, KC_LNG2 = 0x90, 0x91
KC_SCLN = 0x33
MOD_LSFT, MOD_LALT = 0x02, 0x04

# マトリクス上の位置 (keymap.py の ASSIGN より)。
POS_Q = (0, 0)          # レイヤー 0: LT(4, Q)
POS_W = (0, 1)          # レイヤー 0: W
POS_R = (0, 3)          # レイヤー 0: LT(3, R)
POS_Z = (2, 0)          # レイヤー 0: HT(Z, LSFT) = MT(MOD_LSFT, KC_Z)
POS_LANG2 = (3, 3)      # レイヤー 0: HT(LANG2, LALT)
POS_LANG1_PH = (3, 5)   # レイヤー 0: LT(1, LANG1, prefer_hold=True)
POS_LANG1 = (3, 6)      # レイヤー 0: LT(1, LANG1)
POS_TALK = (4, 5)       # レイヤー 0: KC.TALK (独自キー)
POS_SPC = (4, 6)        # レイヤー 0: LT(2, SPC, prefer_hold=True)
POS_VOLDN = (3, 0)      # レイヤー 0: KC.STK_VOLDN (独自キー)

# 独自キーは「何をしたいか」で出る (qmk_port.h の stackee_key_action_t)。
STK_TALK = 'TALK'
STK_VOLDN = 'VOLDN'


def tap(pos, at, hold_ms=40):
    """時刻 at [ms] に押して hold_ms 後に離す台本を返す。"""
    return ('t %d\nd %d %d\nt %d\nu %d %d\n'
            % (at, pos[0], pos[1], at + hold_ms, pos[0], pos[1]))


class SingleKeyTest(unittest.TestCase):
    def test_plain_key_sends_press_then_release(self):
        lines = run('t 50\nmark w\n' + tap(POS_W, 50) + 't 200\n')
        self.assertEqual(after(lines, 'w'), [kb(0, KC_W), NONE])


class DebounceLatencyTest(unittest.TestCase):
    """押下が **その場で** レポートになること (デバウンス待ちが無いこと)。

    ★ 2026-09-16: 実機で押下 → HID 送出が 中央値 5.99 ms あり、設計目標
      (中央値 2 ms、DESIGN.md §3) に届かなかった。原因は QMK の
      debounce/sym_defer_pk (「DEBOUNCE ms 変化しなくなってから確定」)。
      TCA8418 はチップの中でデバウンスしてから FIFO に積むので、そこへ
      重ねる意味が無い。`DEBOUNCE 0` にして素通しにした。

    ここでは 1 ms ずつ時計を進めながら目印を打ち、レポートが押下から
    何 ms 目の区間に出るかを見る。デバウンスが復活したらここが落ちる。
    """

    def ms_until_report(self, pos, steps=12):
        """押下から何 ms 目の区間でレポートが出たかを返す。出なければ None。"""
        script = 't 100\nd %d %d\n' % pos
        for i in range(steps):
            script += 'mark s%d\nt %d\n' % (i, 101 + i)
        lines = run(script)
        for i in range(steps):
            if [line for line in after(lines, 's%d' % i)
                    if line.startswith('KB ')]:
                return i
        return None

    def test_a_press_is_reported_on_the_very_next_scan(self):
        # 0 = 「押した次の 1 ms の走査でもう出ている」。
        self.assertEqual(self.ms_until_report(POS_W), 0,
                         'デバウンス待ちが入っている (DEBOUNCE を確認)')

    def test_a_release_is_reported_on_the_very_next_scan(self):
        script = ('t 100\nd %d %d\nt 110\nu %d %d\n' % (POS_W + POS_W))
        for i in range(6):
            script += 'mark r%d\nt %d\n' % (i, 111 + i)
        lines = run(script)
        # 最初の区間で解放レポート (全部 0) が出ているはず。
        self.assertIn(NONE, after(lines, 'r0'),
                      '解放にデバウンス待ちが入っている')


class HoldTapTest(unittest.TestCase):
    """KMK の HoldTap を QMK の MT / LT に置き換えた結果を確かめる。"""

    def test_mod_tap_below_tapping_term_is_a_tap(self):
        # HT(Z, LSFT) を 194 ms で離す -> Z
        lines = run('t 100\nmark z\n' + tap(POS_Z, 100, 194) + 't 500\n')
        self.assertEqual(after(lines, 'z'), [kb(0, KC_Z), NONE])

    def test_mod_tap_above_tapping_term_is_a_hold(self):
        # 206 ms 押さえたら Shift。TAPPING_TERM = 200 の両側を見る。
        lines = run('t 100\nmark z\n' + tap(POS_Z, 100, 206) + 't 500\n')
        self.assertEqual(after(lines, 'z'), [kb(MOD_LSFT), NONE])

    def test_layer_tap_below_tapping_term_is_a_tap(self):
        # ★ LT の TAPPING_TERM は 300 ms (KMK の Layers インスタンスは
        #   tap_time を既定の 300 のままにしてある)。HT の 200 とは別勘定。
        lines = run('t 100\nmark r\n' + tap(POS_R, 100, 294) + 't 600\n')
        self.assertEqual(after(lines, 'r'), [kb(0, KC_R), NONE])

    def test_layer_tap_above_tapping_term_is_a_hold(self):
        # 306 ms 押さえたらレイヤーに入る。タップ側 (R) は出ない。
        lines = run('t 100\nmark r\n' + tap(POS_R, 100, 306) + 't 700\n')
        self.assertEqual(after(lines, 'r'), [])

    def test_layer_tap_uses_300_not_the_200_of_mod_tap(self):
        # 同じ 250 ms でも、LT はタップ (300 未満)、MT はホールド (200 以上)。
        lt = run('t 100\nmark k\n' + tap(POS_R, 100, 250) + 't 600\n')
        self.assertEqual(after(lt, 'k'), [kb(0, KC_R), NONE])
        mt = run('t 100\nmark k\n' + tap(POS_Z, 100, 250) + 't 600\n')
        self.assertEqual(after(mt, 'k'), [kb(MOD_LSFT), NONE])

    def test_layer_tap_held_switches_layer(self):
        # LT(3, R) を押さえたまま Q の位置を叩くと、レイヤー 3 の Esc が出る。
        script = ('t 100\nmark lt\nd %d %d\nt 500\nd %d %d\nt 520\nu %d %d\n'
                  't 540\nu %d %d\nt 700\n'
                  % (POS_R + POS_Q + POS_Q + POS_R))
        lines = run(script)
        self.assertEqual(after(lines, 'lt'), [kb(0, KC_ESC), NONE])

    def test_prefer_hold_settles_on_other_key_press(self):
        # LT(2, SPC, prefer_hold=True) は他のキーが押された時点でレイヤー確定。
        # TAPPING_TERM より前 (10 ms) に割り込んでもレイヤー 2 になる。
        # レイヤー 2 の W の位置は LSFT(KC_2) = JIS の「"」。
        script = ('t 100\nmark ph\nd %d %d\nt 110\nd %d %d\nt 120\nu %d %d\n'
                  't 130\nu %d %d\nt 400\n'
                  % (POS_SPC + POS_W + POS_W + POS_SPC))
        lines = run(script)
        self.assertEqual(after(lines, 'ph'),
                         [kb(MOD_LSFT), kb(MOD_LSFT, KC_2), kb(MOD_LSFT), NONE])

    def test_without_prefer_hold_an_interruption_does_not_settle(self):
        # LT(1, LANG1) (キー 37、prefer_hold なし) は割り込まれてもタップのまま。
        # KMK の「not tap_interrupted and not prefer_hold -> 割り込まれない」。
        script = ('t 100\nmark noph\nd %d %d\nt 110\nd %d %d\nt 120\nu %d %d\n'
                  't 130\nu %d %d\nt 400\n'
                  % (POS_LANG1 + POS_W + POS_W + POS_LANG1))
        lines = run(script)
        # KMK と同じ順序: LT が離れた時点でタップ (LANG1) を確定し、
        # そのあとに溜めておいた W を流す。W が出るときは LANG1 がまだ
        # 押されたままなので、同時押しのレポートになる。
        self.assertEqual(after(lines, 'noph'),
                         [kb(0, KC_LNG1), kb(0, KC_LNG1, KC_W),
                          kb(0, KC_LNG1), NONE])

    def test_prefer_hold_variant_of_the_same_keycode_differs(self):
        # キー 36 と 37 はどちらも LT(1, LANG1) だが 36 だけ prefer_hold。
        # キーコードが同じでも設定が違うことを確かめる (生成側の要)。
        script = ('t 100\nmark ph36\nd %d %d\nt 110\nd %d %d\nt 120\nu %d %d\n'
                  't 130\nu %d %d\nt 400\n'
                  % (POS_LANG1_PH + POS_W + POS_W + POS_LANG1_PH))
        lines = run(script)
        # レイヤー 1 の W の位置は KC.N2 = 0x1F。LANG1 は出ない。
        out = after(lines, 'ph36')
        self.assertIn(kb(0, KC_2), out)
        self.assertNotIn(kb(0, KC_LNG1), out)


class JisTest(unittest.TestCase):
    """JIS の英数 / かな。adafruit_ble の 0x89 上限問題を再発させない。"""

    def test_lang1_is_0x90(self):
        lines = run('t 100\nmark l1\n' + tap(POS_LANG1, 100, 50) + 't 400\n')
        self.assertEqual(after(lines, 'l1'), [kb(0, KC_LNG1), NONE])

    def test_lang2_is_0x91(self):
        lines = run('t 100\nmark l2\n' + tap(POS_LANG2, 100, 50) + 't 400\n')
        self.assertEqual(after(lines, 'l2'), [kb(0, KC_LNG2), NONE])


class CustomKeyTest(unittest.TestCase):
    """独自キーは HID に 1 バイトも漏れない。"""

    def test_talk_key_does_not_reach_hid(self):
        lines = run('t 100\nmark talk\n' + tap(POS_TALK, 100, 50) + 't 400\n')
        out = after(lines, 'talk')
        self.assertEqual(out, ['CUSTOM %s 1' % STK_TALK,
                               'CUSTOM %s 0' % STK_TALK])
        self.assertFalse([line for line in out if line.startswith('KB ')])

    def test_volume_key_does_not_reach_hid(self):
        lines = run('t 100\nmark vol\n' + tap(POS_VOLDN, 100, 50) + 't 400\n')
        out = after(lines, 'vol')
        self.assertEqual(out, ['CUSTOM %s 1' % STK_VOLDN,
                               'CUSTOM %s 0' % STK_VOLDN])


class ExtendedModTapTest(unittest.TestCase):
    """タップ側が修飾つきの HoldTap (QMK の MT() では表せないもの)。

    レイヤー 1 のキー 21 = KC.HT(KC.LSFT(KC.SCLN), KC.LSFT)。
    離せば JIS の「+」(Shift + ;)、押さえれば Shift。
    """

    def _enter_layer1_and(self, body):
        # LT(1, LANG1, prefer_hold=True) を押さえたままにする。
        # LT の TAPPING_TERM は 300 ms なので、そこを越えてから本題に入る。
        return ('t 100\nd %d %d\nt 500\nmark ext\n' % POS_LANG1_PH) + body

    def test_tap_sends_shifted_semicolon(self):
        # STK_MT_0 の tapping_term は 200 ms (HT 由来)。
        script = self._enter_layer1_and(
            'd %d %d\nt 540\nu %d %d\nt 700\n' % (POS_Z + POS_Z))
        out = after(run(script), 'ext')
        self.assertIn(kb(MOD_LSFT, KC_SCLN), out)

    def test_hold_registers_shift_only(self):
        script = self._enter_layer1_and(
            'd %d %d\nt 800\nu %d %d\nt 1000\n' % (POS_Z + POS_Z))
        out = after(run(script), 'ext')
        self.assertIn(kb(MOD_LSFT), out)
        self.assertNotIn(kb(MOD_LSFT, KC_SCLN), out)


MS_RIGHT = 0x00D0
MS_BTN1 = 0x00D1


def mouse(buttons, x, y, v=0, h=0):
    """期待する MOUSE 行。report_mouse_t = buttons, x, y, v, h の 5 バイト。"""
    def b(n):
        return '%02X' % (n & 0xFF)
    return 'MOUSE %s %s %s %s %s' % (b(buttons), b(x), b(y), b(v), b(h))


def mouse_x(line):
    """MOUSE 行から x の移動量 (符号つき) を取り出す。"""
    value = int(line.split()[2], 16)
    return value - 256 if value > 127 else value


class MouseKeyTest(unittest.TestCase):
    """QMK のマウスキー。現行 CircuitPython 版も KMK の MouseKeys を入れている
    (code.py が keyboard.modules.append(MouseKeys()))。

    既定配列にはマウスキーが 1 つも無いので、VIA と同じ道 (dynamic keymap) で
    空きスロットに割り当ててから試す。
    """

    def test_holding_right_keeps_moving_and_accelerates(self):
        # レイヤー 0 の (0,1) を MS_RIGHT にして 500 ms 押さえる。
        script = ('kc 0 1 %d\nt 100\nmark ms\nd %d %d\nt 600\nu %d %d\nt 700\n'
                  % ((MS_RIGHT,) + POS_W + POS_W))
        out = [line for line in after(run(script), 'ms')
               if line.startswith('MOUSE ')]
        self.assertGreater(len(out), 5, '押している間ずっと動き続けるはず')
        steps = [mouse_x(line) for line in out if mouse_x(line) != 0]
        self.assertTrue(all(step > 0 for step in steps), '右へ動くはず')
        # QMK 既定は加速つき。最初の 1 歩より、後のほうが大きい。
        self.assertGreater(max(steps), steps[0],
                           '加速していない (QMK 既定は加速つき)')
        # 離したら 0 に戻る。
        self.assertEqual(mouse_x(out[-1]), 0)

    def test_button_press_and_release_show_up_in_the_report(self):
        script = ('kc 0 1 %d\nt 100\nmark mb\n' % MS_BTN1
                  + tap(POS_W, 100, 50) + 't 400\n')
        out = [line for line in after(run(script), 'mb')
               if line.startswith('MOUSE ')]
        self.assertEqual(out, [mouse(0x01, 0, 0), mouse(0x00, 0, 0)])

    def test_mouse_keys_do_not_disturb_normal_keys_or_custom_keys(self):
        # マウスボタンを押したまま W を打ち、独自キーも挟む。
        script = ('kc 0 0 %d\nt 100\nmark mix\n'
                  'd 0 0\nt 150\nd %d %d\nt 170\nu %d %d\n'
                  't 190\nd %d %d\nt 210\nu %d %d\n'
                  't 240\nu 0 0\nt 400\n'
                  % ((MS_BTN1,) + POS_W + POS_W + POS_TALK + POS_TALK))
        out = after(run(script), 'mix')
        self.assertIn(mouse(0x01, 0, 0), out)       # ボタン押下
        self.assertIn(kb(0, KC_W), out)             # 通常キーはそのまま出る
        self.assertIn('CUSTOM %s 1' % STK_TALK, out)
        self.assertIn(mouse(0x00, 0, 0), out)       # ボタン解放


KC_F24 = 0x0073
POS_INJECT = (4, 0)     # 配線の無いスロット。key.inject が使う場所


class KeyInjectTest(unittest.TestCase):
    """コンソールの key.inject が通る道を、そのまま確かめる。

    実機側は「レイヤー 0 の空きスロット (4,0) に一時的にキーコードを置き、
    TCA8418 のイベントとして押して離し、終わったら KC_NO に戻す」。
    ここでは同じ手順を台本で再現する。**通常のキー処理と送信キューを
    通る**ことが要点で、迂回すると「テストは通るのに実機で打てない」に
    なる。
    """

    def test_injecting_f24_produces_press_and_release_reports(self):
        script = ('kc %d %d %d\nt 100\nmark inj\n' % (POS_INJECT + (KC_F24,))
                  + tap(POS_INJECT, 100, 30) + 't 300\n')
        out = after(run(script), 'inj')
        self.assertEqual(out, [kb(0, KC_F24), NONE])

    def test_the_slot_is_empty_again_after_restoring_it(self):
        # 実機は終わったら KC_NO に戻す。戻したあとは何も出ない。
        script = ('kc %d %d %d\nt 100\n' % (POS_INJECT + (KC_F24,))
                  + tap(POS_INJECT, 100, 30)
                  + 't 300\nkc %d %d 0\nmark after\n' % POS_INJECT
                  + tap(POS_INJECT, 300, 30) + 't 500\n')
        self.assertEqual(after(run(script), 'after'), [])

    def test_an_unwired_slot_sends_nothing_by_default(self):
        # 既定配列では (4,0) は KC_NO。注入していないのに何か出たら、
        # 配線表かキーマップ生成が壊れている。
        script = 't 100\nmark none\n' + tap(POS_INJECT, 100, 30) + 't 300\n'
        self.assertEqual(after(run(script), 'none'), [])


class MatrixTest(unittest.TestCase):
    def test_fifo_overflow_releases_everything(self):
        # TCA8418 の FIFO が溢れたら押下中を全解放する (スタックキー対策)。
        script = ('t 100\nmark ovf\nd %d %d\nt 150\novf\nt 300\n' % POS_W)
        out = after(run(script), 'ovf')
        self.assertEqual(out, [kb(0, KC_W), NONE])


class ViaTest(unittest.TestCase):
    """Raw HID (VIA) の受け答え。"""

    def test_protocol_version(self):
        # id_get_protocol_version = 0x01。応答の 2-3 バイト目が版。
        out = run('t 10\nmark via\nv 01\nt 50\n')
        raw = [line for line in after(out, 'via') if line.startswith('RAW ')]
        self.assertEqual(len(raw), 1)
        self.assertEqual(raw[0].split()[1], '01')

    def test_layer_count_is_six(self):
        # id_dynamic_keymap_get_layer_count = 0x11
        out = run('t 10\nmark via\nv 11\nt 50\n')
        raw = [line for line in after(out, 'via') if line.startswith('RAW ')][0]
        self.assertEqual(raw.split()[1:3], ['11', '06'])

    def test_default_keymap_is_readable_over_via(self):
        # id_dynamic_keymap_get_keycode = 0x04、layer 0 / row 0 / col 1 = KC_W
        out = run('t 10\nmark via\nv 04 00 00 01\nt 50\n')
        raw = [line for line in after(out, 'via') if line.startswith('RAW ')][0]
        parts = raw.split()[1:6]
        self.assertEqual(parts[:4], ['04', '00', '00', '01'])
        self.assertEqual(int(parts[4], 16), 0x00)     # 上位バイト
        self.assertEqual(int(raw.split()[6], 16), KC_W)

    def test_remapping_a_key_changes_what_it_sends(self):
        # VIA で row0/col1 を KC_E に書き換えたら E が出る。書き換えは
        # NVS (ホストビルドでは RAM) に落ちるところまで確かめる。
        script = ('t 10\nv 05 00 00 01 00 %02X\nt 100\nmark after\n' % KC_E
                  + tap(POS_W, 100) + 't 600\nnvs\n')
        lines = run(script)
        out = after(lines, 'after')
        self.assertIn(kb(0, KC_E), out)
        self.assertIn('NVS saves=1 valid=1',
                      [line for line in lines if line.startswith('NVS ')])

    def test_writes_are_batched_not_one_flash_write_per_key(self):
        # 10 キーぶん書き換えても NVS への書き戻しは 1 回にまとまる。
        script = 't 10\n'
        for col in range(10):
            script += 'v 05 00 00 %02X 00 %02X\n' % (col, KC_E)
            script += 't %d\n' % (20 + col * 5)
        script += 't 600\nnvs\n'
        lines = run(script)
        self.assertIn('NVS saves=1 valid=1',
                      [line for line in lines if line.startswith('NVS ')])


class BootloaderTest(unittest.TestCase):
    """QK_BOOT (KMK の KC.RESET と同じ位置) が脱出路へ行く。"""

    def test_reset_key_goes_to_rom_download(self):
        # レイヤー 5 は レイヤー 4 (Q 長押し) -> MO(5) で入る。
        # Q の位置 = LT(4, Q)、レイヤー 4 の キー 29 の位置 (2,8) = MO(5)、
        # レイヤー 5 の キー 26 の位置 (2,5) = QK_BOOT。
        script = ('t 100\nd 0 0\nt 500\nd 2 8\nt 550\nmark boot\n'
                  'd 2 5\nt 600\n')
        self.assertIn('BOOTLOADER', after(run(script), 'boot'))


if __name__ == '__main__':
    unittest.main(verbosity=2)
