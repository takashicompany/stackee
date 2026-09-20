#!/usr/bin/env python3
"""段階 4 のタッチパッドを Mac 上で突き合わせる。**実機には触らない。**

確かめたいのは 1 つだけ:

> **C 版 (main/stackee_touch_core.c) が、現行 CircuitPython 版
> (firmware/kmk/stackee_touch.py の TouchpadMouse) と同じ判断をするか。**

だから期待値は手で書かない。CircuitPython / KMK の代わりのスタブを
差し込んで `stackee_touch.py` を **そのまま import** し、同じ座標列を
両方に流して、出てくる「ポインタ移動 / スクロール / クリック」を
1 つずつ比べる。

★ 1 か所だけ**わざと**違う: 端数の丸め方。
  Python の round() は偶数丸め (round(0.5) == 0)、C 版は 0 から遠いほうへ
  (0.5 -> 1)。操作感には出ない 1 カウントの差なので、ちょうど .5 になった
  標本だけは ±1 を許す (それ以外は 1 も違ってはいけない)。

  python3 firmware/tools/test_touch_host.py
  python3 -m pytest firmware/tools/test_touch_host.py
"""
import importlib.util
import os
import subprocess
import sys
import tempfile
import types
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
IDF = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import stackee_tree as tree             # noqa: E402

# 期待値は移植元 (現行 CircuitPython 版の stackee_touch.py) から出す。
if tree.KMK is None:
    raise unittest.SkipTest(
        '移植元 (firmware/kmk) が無いので突き合わせられない')
KMK = str(tree.KMK)

SOURCES = ['stackee_touch_core.c']
# ★ ASan + UBSan つき。段階 3 の登録簿で入れ物の外へ書く欠陥を
#   これで初めて捕まえた (test_cfg_host.py の注意書きと同じ)。
SANITIZE = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']

_BINARY = None


def binary():
    global _BINARY
    if _BINARY is None:
        out = os.path.join(tempfile.mkdtemp(), 'touch')
        cmd = (['cc', '-O1', '-std=gnu11', '-Wall', '-Werror'] + SANITIZE + [
                '-I', os.path.join(IDF, 'main'), '-o', out,
                os.path.join(IDF, 'hostbuild/touch_main.c')]
               + [os.path.join(IDF, 'main', s) for s in SOURCES]
               + ['-lm'])
        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError('ホストビルドに失敗:\n' + build.stderr)
        _BINARY = out
    return _BINARY


def run_c(script, args=()):
    out = subprocess.run([binary()] + list(args), input=script,
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise AssertionError('実行に失敗:\n' + out.stderr)
    rows = []
    for line in out.stdout.splitlines():
        rows.append(line.split('\t'))
    return rows


# ---------------------------------------------------------------------------
# 現行 CircuitPython 版を import するためのスタブ
# ---------------------------------------------------------------------------
# stackee_touch.py が要るのは kmk.keys (AX / KC / make_key)、
# kmk.kmktime (PeriodicTimer / ticks_*)、kmk.modules (Module) の 3 つだけ。
# 実物の KMK は CircuitPython 専用なので、ここで最小の偽物を差し込む。

CLOCK = {'t': 0}


def _ticks_ms():
    return CLOCK['t']


def _ticks_diff(new, start):
    return new - start


class _Axis:
    """AX.X / AX.Y / AX.P / AX.W の代わり。move() の呼ばれ方を記録する。"""

    def __init__(self, name, log):
        self.name = name
        self.log = log

    def move(self, keyboard, delta):
        self.log.append((self.name, delta))


class _PeriodicTimer:
    """毎回 True を返す (座標を流すたびに 1 回判定させる)。"""

    def __init__(self, interval):
        self.interval = interval

    def tick(self):
        return True


def install_kmk(log):
    kmk = types.ModuleType('kmk')
    sys.modules['kmk'] = kmk

    keys = types.ModuleType('kmk.keys')
    ax = types.SimpleNamespace(X=_Axis('X', log), Y=_Axis('Y', log),
                               P=_Axis('P', log), W=_Axis('W', log))
    kc = types.SimpleNamespace(MB_LMB='MB_LMB', MB_RMB='MB_RMB')
    keys.AX = ax
    keys.KC = kc
    keys.make_key = lambda **kw: None
    sys.modules['kmk.keys'] = keys
    kmk.keys = keys

    kmktime = types.ModuleType('kmk.kmktime')
    kmktime.PeriodicTimer = _PeriodicTimer
    kmktime.ticks_ms = _ticks_ms
    kmktime.ticks_diff = _ticks_diff
    sys.modules['kmk.kmktime'] = kmktime
    kmk.kmktime = kmktime

    modules = types.ModuleType('kmk.modules')

    class Module:
        pass

    modules.Module = Module
    sys.modules['kmk.modules'] = modules
    kmk.modules = modules
    return ax


LOG = []
install_kmk(LOG)

spec = importlib.util.spec_from_file_location(
    'kmk_touch', os.path.join(KMK, 'stackee_touch.py'))
TOUCH = importlib.util.module_from_spec(spec)
sys.modules['kmk_touch'] = TOUCH
spec.loader.exec_module(TOUCH)


class _Keyboard:
    """KMK の keyboard の代わり。押した / 離したキーだけ数える。"""

    def __init__(self, log):
        self.log = log
        self.timeouts = []

    def add_key(self, key):
        self.log.append(('click', key))

    def remove_key(self, key):
        pass

    def set_timeout(self, ms, fn):
        self.timeouts.append((ms, fn))


def run_py(script, **kwargs):
    """同じ台本を現行 CircuitPython 版に流して、出た手を並べる。"""
    LOG.clear()
    CLOCK['t'] = 1
    pad = TOUCH.TouchpadMouse(i2c=None, **kwargs)
    pad._timer = _PeriodicTimer(pad.update_interval)
    pad.touch = object()        # None でなければよい (読みはこちらでやる)
    kb = _Keyboard(LOG)

    for line in script.splitlines():
        line = line.strip()
        if not line:
            continue
        kind = line[0]
        rest = line[1:].split()
        if kind == 's':
            pad.scroll_mode = rest[0] == '1'
            if not pad.scroll_mode:
                pad.reset_motion()
        elif kind == 'p':
            x, y, dt = int(rest[0]), int(rest[1]), int(rest[2])
            CLOCK['t'] += dt
            pad._handle_touch(kb, (x, y))
        elif kind == 'r':
            CLOCK['t'] += int(rest[0]) if rest else 0
            pad._handle_release(kb)
    return list(LOG)


def c_events(rows):
    """C 版の出力を、Python 版と同じ形 (('X', 3), ('click', 'MB_LMB')) に直す。"""
    out = []
    for row in rows:
        if row[0] == 'move':
            dx, dy = int(row[1]), int(row[2])
            # ★ Python 版は 0 でない軸だけ move() を呼ぶ。同じ順 (X -> Y)。
            if dx:
                out.append(('X', dx))
            if dy:
                out.append(('Y', dy))
        elif row[0] == 'scroll':
            v, h = int(row[1]), int(row[2])
            # Python 版は横 (P) -> 縦 (W) の順に呼ぶ。
            if h:
                out.append(('P', h))
            if v:
                out.append(('W', v))
        elif row[0] == 'click':
            out.append(('click', 'MB_RMB' if row[1] == '2' else 'MB_LMB'))
    return out


def close_enough(a, b):
    """1 手ずつ比べる。端数の丸めの違い (±1) だけ許す。"""
    if len(a) != len(b):
        return False
    for (an, av), (bn, bv) in zip(a, b):
        if an != bn:
            return False
        if an == 'click':
            if av != bv:
                return False
        elif abs(int(av) - int(bv)) > 1:
            return False
    return True


# ---------------------------------------------------------------------------
class TouchTest(unittest.TestCase):

    def check(self, script, args=(), **kwargs):
        got = c_events(run_c(script, args))
        want = run_py(script, **kwargs)
        self.assertTrue(close_enough(got, want),
                        'C と CircuitPython で結果が違う\n'
                        'C  : %r\nKMK: %r\n台本:\n%s' % (got, want, script))
        return got

    # ---- なぞる --------------------------------------------------------
    def test_drag_right_down(self):
        script = ''.join('p %d %d 5\n' % (100 + i * 7, 80 + i * 3)
                         for i in range(12))
        got = self.check(script)
        self.assertTrue(any(n in ('X', 'Y') for n, _v in got),
                        'なぞったのに 1 回も動いていない')

    def test_drag_left_up(self):
        script = ''.join('p %d %d 5\n' % (250 - i * 9, 200 - i * 4)
                         for i in range(12))
        self.check(script)

    def test_drag_then_release(self):
        script = ''.join('p %d %d 5\n' % (100 + i * 20, 80) for i in range(8))
        script += 'r 5\n'
        self.check(script)

    # ---- 回転と反転 ----------------------------------------------------
    def test_rotation_0(self):
        script = ''.join('p %d %d 5\n' % (100 + i * 6, 80 + i * 6)
                         for i in range(10))
        self.check(script, args=['rotation=0'], rotation=0)

    def test_rotation_90(self):
        script = ''.join('p %d %d 5\n' % (100 + i * 6, 80 + i * 6)
                         for i in range(10))
        self.check(script, args=['rotation=90'], rotation=90)

    def test_rotation_180(self):
        script = ''.join('p %d %d 5\n' % (100 + i * 6, 80 + i * 6)
                         for i in range(10))
        self.check(script, args=['rotation=180'], rotation=180)

    def test_no_pointer_invert(self):
        script = ''.join('p %d %d 5\n' % (100 + i * 6, 80 + i * 6)
                         for i in range(10))
        self.check(script,
                   args=['pointer_invert_x=0', 'pointer_invert_y=0'],
                   pointer_invert_x=False, pointer_invert_y=False)

    def test_shared_invert(self):
        script = ''.join('p %d %d 5\n' % (100 + i * 6, 80 + i * 6)
                         for i in range(10))
        self.check(script, args=['invert_x=1', 'invert_y=1'],
                   invert_x=True, invert_y=True)

    # ---- タップ --------------------------------------------------------
    # ★ 向きの効き方 (実測と一致): rotation 270 なので画面 X = 生 y。
    #   さらに pointer_invert_x で 239 - y になる。つまり
    #     生 y = 220 -> 画面 X 19   -> 左クリック
    #     生 y = 30  -> 画面 X 209  -> 右 30% -> 右クリック
    def test_tap_left(self):
        self.check('p 200 220 5\np 200 220 40\nr 5\n')

    def test_tap_right_zone(self):
        self.check('p 200 30 5\np 200 30 40\nr 5\n')

    def test_tap_too_short(self):
        # 幽霊タッチ (1 フレームだけ) は却下される。
        got = c_events(run_c('p 200 220 5\nr 5\n'))
        self.assertEqual([], [e for e in got if e[0] == 'click'])
        self.check('p 200 220 5\nr 5\n')

    def test_tap_too_long(self):
        self.check('p 200 220 5\np 200 220 400\nr 5\n')

    def test_tap_moved_too_far(self):
        script = 'p 100 100 5\n'
        script += ''.join('p %d %d 5\n' % (100 + i * 20, 100) for i in range(1, 8))
        script += 'r 5\n'
        self.check(script)

    def test_tap_boundary_min(self):
        # ちょうど 10 ms (tap_min_time) で通る / 9 ms で落ちる。
        # 生 y = 30 は右 30% なのでボタンは 2。
        rows = run_c('p 200 30 0\np 200 30 10\nr 0\n')
        self.assertIn(['click', '2'], rows)
        rows = run_c('p 200 30 0\np 200 30 9\nr 0\n')
        self.assertFalse(any(r[0] == 'click' for r in rows))

    def test_tap_boundary_max(self):
        rows = run_c('p 200 30 0\np 200 30 200\nr 0\n')
        self.assertTrue(any(r[0] == 'click' for r in rows))
        rows = run_c('p 200 30 0\np 200 30 201\nr 0\n')
        self.assertFalse(any(r[0] == 'click' for r in rows))

    def test_right_zone_disabled(self):
        # 右クリック領域を 0 にすると、右寄りのタップでも左クリックになる。
        got = c_events(run_c('p 200 30 5\np 200 30 40\nr 5\n',
                             ['right_click_zone=0']))
        self.assertIn(('click', 'MB_LMB'), got)
        self.check('p 200 30 5\np 200 30 40\nr 5\n',
                   args=['right_click_zone=0'], right_click_zone=0)

    # ---- スクロール ----------------------------------------------------
    def test_scroll(self):
        script = 's 1\n'
        script += ''.join('p %d %d 5\n' % (100, 80 + i * 25) for i in range(20))
        self.check(script)

    def test_scroll_horizontal(self):
        script = 's 1\n'
        script += ''.join('p %d %d 5\n' % (100 + i * 25, 80) for i in range(20))
        self.check(script)

    def test_scroll_off_resets(self):
        script = 's 1\n'
        script += ''.join('p %d %d 5\n' % (100, 80 + i * 25) for i in range(10))
        script += 's 0\n'
        script += ''.join('p %d %d 5\n' % (100, 80 + i * 25) for i in range(10))
        self.check(script)

    # ---- 乱数でなぞる (再現できるように種を固定) -----------------------
    def test_random_drags(self):
        import random
        rnd = random.Random(20260916)
        for trial in range(20):
            x, y = rnd.randrange(20, 300), rnd.randrange(20, 220)
            script = ''
            for _ in range(rnd.randrange(3, 25)):
                x = max(0, min(319, x + rnd.randrange(-18, 19)))
                y = max(0, min(239, y + rnd.randrange(-18, 19)))
                script += 'p %d %d %d\n' % (x, y, rnd.randrange(1, 40))
            script += 'r %d\n' % rnd.randrange(1, 20)
            self.check(script)

    # ---- FT6336 の生バイトの読み方 -------------------------------------
    def test_parse_one_point(self):
        # レジスタ 0x02 = 1 点。0x03 から xh,xl,yh,yl。
        raw = ['00'] * 16
        raw[2] = '01'
        raw[3] = '81'       # 上位 2 ビットはイベントフラグ。0x0F でマスクされる
        raw[4] = '23'       # x = 0x123 = 291
        raw[5] = '00'
        raw[6] = '45'       # y = 0x045 = 69
        rows = run_c('x ' + ''.join(raw) + '\n')
        self.assertEqual(['parse', '1', '291', '69', '1'], rows[0])

    def test_parse_no_point(self):
        raw = ['00'] * 16
        rows = run_c('x ' + ''.join(raw) + '\n')
        self.assertEqual(['parse', '0', '-1', '-1', '0'], rows[0])

    def test_parse_ghost_ffff(self):
        # 0xFFFF / 0xFFFF は幽霊。1 点目が幽霊なら 2 点目を見る。
        raw = ['00'] * 16
        raw[2] = '02'
        raw[3] = raw[4] = raw[5] = raw[6] = 'ff'
        raw[9] = '00'
        raw[10] = '10'      # x = 16
        raw[11] = '00'
        raw[12] = '20'      # y = 32
        rows = run_c('x ' + ''.join(raw) + '\n')
        self.assertEqual(['parse', '1', '16', '32', '2'], rows[0])

    def test_parse_count_clamped(self):
        # 3 点以上と言われても 2 点しか読まない (読んだのは 15 バイトだけ)。
        raw = ['00'] * 16
        raw[2] = '05'
        raw[3] = '00'
        raw[4] = '01'
        raw[5] = '00'
        raw[6] = '02'
        rows = run_c('x ' + ''.join(raw) + '\n')
        self.assertEqual('2', rows[0][4])

    # ---- 数え方 --------------------------------------------------------
    def test_stats(self):
        # 1 回目 = 右 30% で 40 ms のタップ (通る)、2 回目 = 1 フレーム (却下)。
        rows = run_c('p 200 30 0\np 200 30 40\nr 0\np 10 10 0\nr 0\n')
        stats = [r for r in rows if r[0] == 'stats'][0]
        # points=3, releases=2, taps_left=0, taps_right=1, rejected=1
        self.assertEqual(['stats', '3', '2', '0', '1', '1'], stats)


if __name__ == '__main__':
    unittest.main(verbosity=2)
