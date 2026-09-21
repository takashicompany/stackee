#!/usr/bin/env python3
"""マイクを開けるとき ES7210 を全設定し直すかの判断。**実機に触らない。**

★ **なぜこれがあるのか。** 押すたびに ES7210 の全レジスタを書き直していて、
  それだけで **I2C に 44 ms** かかっていた (実機で測った。押下 → 使える音まで
  約 144 ms のうちの 44 ms)。レジスタは電源を切らないかぎり残るので、
  **全設定は 1 回で足りる**。ただし「1 回書いたからもう安心」にはしない —
  ほかの経路が触ったかもしれないときは印を立てて書き直す。

見るのは 4 つ:
  ・1 回目は全設定、2 回目からは電源だけ
  ・印を立てたら次の 1 回だけ全設定に戻る
  ・書いた値が残っていなければ (alive=false) 全設定に戻る
  ・失敗したら次は必ず全設定 (古い設定のまま録らない)

  python3 firmware/tools/test_micopen_host.py
"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

IDF = Path(__file__).resolve().parents[1]
SANITIZE = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']

_BINARY = None

# 判断だけを取り出した小さな台本の実行器。ここに書いて毎回コンパイルする。
MAIN_C = r'''
#include <stdio.h>
#include <string.h>
#include "stackee_micopen.h"

int main(void) {
    stackee_micopen_t m;
    stackee_micopen_reset(&m);
    char line[64];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        char cmd[32];
        int a = 0, b = 0;
        int n = sscanf(line, "%31s %d %d", cmd, &a, &b);
        if (n < 1) { continue; }
        if (strcmp(cmd, "reset") == 0) {
            stackee_micopen_reset(&m);
        } else if (strcmp(cmd, "invalidate") == 0) {
            stackee_micopen_invalidate(&m);
        } else if (strcmp(cmd, "open") == 0) {
            /* a = alive (書いた値が残っているか), b = ok (開けたか) */
            bool full = stackee_micopen_needs_full(&m, a != 0);
            stackee_micopen_done(&m, full, b != 0);
            printf("OPEN %s ok=%d full=%u light=%u dirty=%d\n",
                   full ? "FULL" : "LIGHT", b != 0,
                   (unsigned)m.full, (unsigned)m.light,
                   (m.dirty || !m.configured) ? 1 : 0);
        } else {
            fprintf(stderr, "unknown: %s\n", cmd);
            return 2;
        }
    }
    return 0;
}
'''


def binary():
    global _BINARY
    if _BINARY is None:
        tmp = Path(tempfile.mkdtemp())
        src = tmp / 'micopen_main.c'
        src.write_text(MAIN_C)
        out = tmp / 'micopen'
        cmd = (['cc', '-O1', '-std=gnu11', '-Wall', '-Werror'] + SANITIZE +
               ['-I', str(IDF / 'main'), '-o', str(out), str(src),
                str(IDF / 'main/stackee_micopen.c')])
        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError('ホストビルドに失敗:\n' + build.stderr)
        _BINARY = out
    return _BINARY


def run(script):
    out = subprocess.run([str(binary())], input=script,
                         capture_output=True, text=True)
    if out.returncode != 0:
        raise AssertionError('実行に失敗:\n' + out.stderr)
    rows = []
    for line in out.stdout.splitlines():
        parts = line.split()
        if parts and parts[0] == 'OPEN':
            rows.append({
                'how': parts[1],
                'ok': int(parts[2].split('=')[1]),
                'full': int(parts[3].split('=')[1]),
                'light': int(parts[4].split('=')[1]),
                'dirty': int(parts[5].split('=')[1]),
            })
    return rows


def opens(n, alive=1, ok=1):
    return ''.join('open %d %d\n' % (alive, ok) for _ in range(n))


class MicOpenTest(unittest.TestCase):
    def test_the_first_open_is_full_and_the_rest_are_light(self):
        rows = run(opens(5))
        self.assertEqual([r['how'] for r in rows],
                         ['FULL', 'LIGHT', 'LIGHT', 'LIGHT', 'LIGHT'])
        # 全設定は 1 回きり。あとは電源を上げ直すだけ。
        self.assertEqual(rows[-1]['full'], 1)
        self.assertEqual(rows[-1]['light'], 4)
        self.assertEqual(rows[-1]['dirty'], 0)

    def test_invalidate_forces_one_full_setup(self):
        rows = run(opens(2) + 'invalidate\n' + opens(3))
        self.assertEqual([r['how'] for r in rows],
                         ['FULL', 'LIGHT', 'FULL', 'LIGHT', 'LIGHT'])
        self.assertEqual(rows[-1]['full'], 2)

    def test_invalidate_twice_still_costs_one_full_setup(self):
        rows = run(opens(1) + 'invalidate\ninvalidate\n' + opens(2))
        self.assertEqual([r['how'] for r in rows], ['FULL', 'FULL', 'LIGHT'])

    def test_a_chip_that_lost_its_registers_is_set_up_again(self):
        # alive=0 = 書いた値が残っていない (化けた / 電源が落ちた)。
        rows = run(opens(2) + 'open 0 1\n' + opens(2))
        self.assertEqual([r['how'] for r in rows],
                         ['FULL', 'LIGHT', 'FULL', 'LIGHT', 'LIGHT'])

    def test_a_failed_open_is_retried_from_scratch(self):
        # ★ 転んだら次は必ず全設定。古い設定のまま録らない。
        rows = run(opens(2) + 'open 1 0\n' + opens(2))
        self.assertEqual([r['how'] for r in rows],
                         ['FULL', 'LIGHT', 'LIGHT', 'FULL', 'LIGHT'])
        # 失敗した直後は印が立っている。
        self.assertEqual(rows[2]['dirty'], 1)
        self.assertEqual(rows[3]['dirty'], 0)

    def test_a_failed_first_open_does_not_pretend_to_be_configured(self):
        rows = run('open 1 0\n' + opens(2))
        self.assertEqual([r['how'] for r in rows], ['FULL', 'FULL', 'LIGHT'])

    def test_reset_starts_over(self):
        rows = run(opens(3) + 'reset\n' + opens(2))
        self.assertEqual([r['how'] for r in rows],
                         ['FULL', 'LIGHT', 'LIGHT', 'FULL', 'LIGHT'])
        self.assertEqual(rows[-1]['full'], 1)   # 数えも 0 に戻る

    def test_the_counters_add_up(self):
        rows = run(opens(10) + 'invalidate\n' + opens(10))
        self.assertEqual(rows[-1]['full'] + rows[-1]['light'], 20)
        self.assertEqual(rows[-1]['full'], 2)


if __name__ == '__main__':
    unittest.main(verbosity=2)
