#!/usr/bin/env python3
"""HTTPS の接続の使い回し (main/stackee_httpcore.c) を Mac 上で確かめる。**実機に触らない。**

本物の決まりを Mac 用にビルドし、偽の「接続」を 1 本つないで台本を流す
(hostbuild/httpcore_main.c)。見ている約束 (README §17-2d):

  * 2 本目からは張らずに送る (使い回し)。張るのは最初と、捨てたあとだけ
  * 送る前の点検で相手が閉じていたら捨てて張り直す
  * 読み残し (本文の途中で切れた・上限超え・打ち切り・失敗) があれば必ず閉じる。
    Connection: close でも閉じる
  * GET は送信・頭・本文の失敗で 1 回だけ送り直す。張れない・待ち時間切れ・
    上限超え・打ち切りは送り直さない
  * POST は送り直さない。古い接続 (30 秒超) には POST を載せない
  * 打ち切りは、送る前ならそのまま返す (接続は残る)。送ったあとは
    ロングポーリングだけが従う

  python3 firmware/tools/test_httpcore_host.py
"""
import os
import re
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
IDF = os.path.dirname(HERE)
SANITIZE = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']

_BINARY = None


def binary():
    global _BINARY
    if _BINARY is None:
        out = os.path.join(tempfile.mkdtemp(), 'httpcore')
        cmd = (['cc', '-O1', '-std=gnu11', '-Wall', '-Werror'] + SANITIZE +
               ['-I', os.path.join(IDF, 'main'), '-o', out,
                os.path.join(IDF, 'hostbuild/httpcore_main.c'),
                os.path.join(IDF, 'main/stackee_httpcore.c')])
        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError('ホストビルドに失敗:\n' + build.stderr)
        _BINARY = out
    return _BINARY


def run(script):
    out = subprocess.run([binary()], input=script, capture_output=True, text=True)
    if out.returncode != 0:
        raise AssertionError('実行に失敗:\n' + out.stderr + out.stdout)
    return out.stdout


def results(text):
    rows = []
    for m in re.finditer(r'^RESULT (.*)$', text, re.M):
        rows.append(dict(kv.split('=', 1) for kv in m.group(1).split()))
    return rows


def counts(text):
    m = re.findall(r'^COUNTS (.*)$', text, re.M)
    assert m, text
    return dict(kv.split('=', 1) for kv in m[-1].split())


def opens(text):
    return re.findall(r'^OPEN (\S+)', text, re.M)


class ReuseTest(unittest.TestCase):
    def test_second_request_reuses(self):
        out = run('resp 200 10\nresp 200 20\nresp 200 30\n'
                  'run GET /inbox\nt 1000\nrun GET /inbox?after=1&wait=25 long\n'
                  't 1000\nrun GET /jobs/a/audio cap=100\ncounts\n')
        self.assertEqual(opens(out), ['fresh', 'reused', 'reused'])
        rs = results(out)
        self.assertEqual([r['reused'] for r in rs], ['0', '1', '1'])
        self.assertEqual([r['got'] for r in rs], ['10', '20', '30'])
        self.assertEqual([r['kept'] for r in rs], ['1', '1', '1'])
        # 張った時間は張ったときだけ出る
        self.assertEqual(rs[0]['connect_ms'], '1500')
        self.assertEqual(rs[1]['connect_ms'], '0')
        c = counts(out)
        self.assertEqual((c['connects'], c['reused'], c['conn']), ('1', '2', '1'))
        self.assertNotIn('CLOSE', out)

    def test_post_reuses_recent_connection(self):
        out = run('resp 200 10\nresp 202 40\n'
                  'run GET /inbox\nt 5000\nrun POST /talk body=100\ncounts\n')
        self.assertEqual(opens(out), ['fresh', 'reused'])
        self.assertEqual(results(out)[1]['status'], '202')
        self.assertIn('WRITE 50', out)      # 本文を書き足していく道を通る

    def test_post_does_not_reuse_old_connection(self):
        # 30 秒を超えて使っていない接続には POST を載せない (黙って死んでいる
        # かもしれず、POST は送り直さないため)。
        out = run('resp 200 10\nresp 202 40\n'
                  'run GET /inbox\nt 30001\nrun POST /talk body=100\ncounts\n')
        self.assertEqual(opens(out), ['fresh', 'fresh'])
        self.assertEqual(results(out)[1]['stale'], '1')
        self.assertEqual(counts(out)['stale'], '1')

    def test_get_reuses_old_connection(self):
        # GET は古くても使う (外れたら 1 回送り直せる)。
        out = run('resp 200 10\nresp 200 10\n'
                  'run GET /inbox\nt 600000\nrun GET /inbox\n')
        self.assertEqual(opens(out), ['fresh', 'reused'])

    def test_connection_close_is_honoured(self):
        out = run('resp 200 10 close\nresp 200 10\n'
                  'run GET /a\nrun GET /b\ncounts\n')
        self.assertEqual(opens(out), ['fresh', 'fresh'])
        self.assertEqual(results(out)[0]['kept'], '0')
        self.assertEqual(out.count('CLOSE'), 1)


class PeerClosedTest(unittest.TestCase):
    def test_peer_closed_is_found_before_sending(self):
        out = run('resp 200 10\nresp 200 10\n'
                  'run GET /a\npeerclose\nrun GET /b\ncounts\n')
        self.assertEqual(opens(out), ['fresh', 'fresh'])
        rs = results(out)
        self.assertEqual((rs[1]['err'], rs[1]['stale'], rs[1]['retried']),
                         ('ok', '1', '0'))
        self.assertEqual(counts(out)['stale'], '1')

    def test_peer_closed_before_post_reconnects_without_resend(self):
        out = run('resp 200 10\nresp 202 10\n'
                  'run GET /a\npeerclose\nrun POST /talk body=10\ncounts\n')
        self.assertEqual(opens(out), ['fresh', 'fresh'])
        r = results(out)[1]
        self.assertEqual((r['err'], r['retried'], r['status']), ('ok', '0', '202'))


class LeftoverCloseTest(unittest.TestCase):
    def test_overflow_closes(self):
        out = run('resp 200 5000\nresp 200 10\n'
                  'run GET /big cap=100\nrun GET /next\ncounts\n')
        rs = results(out)
        self.assertEqual((rs[0]['err'], rs[0]['retried']), ('overflow', '0'))
        self.assertEqual(opens(out), ['fresh', 'fresh'])   # 読み残しは使わない
        self.assertEqual(counts(out)['leftover'], '1')

    def test_exact_fit_is_not_overflow(self):
        out = run('resp 200 100\nresp 200 10\nrun GET /a cap=100\nrun GET /b\n')
        rs = results(out)
        self.assertEqual((rs[0]['err'], rs[0]['got'], rs[0]['kept']), ('ok', '100', '1'))
        self.assertEqual(opens(out), ['fresh', 'reused'])

    def test_exact_fit_chunked_is_not_overflow(self):
        # chunked はちょうど埋まっても終わりの印がまだ。1 バイト読んで確かめる
        out = run('resp 200 100 chunked\nresp 200 10\nrun GET /a cap=100\nrun GET /b\n')
        rs = results(out)
        self.assertEqual((rs[0]['err'], rs[0]['got'], rs[0]['kept']), ('ok', '100', '1'))
        out = run('resp 200 101 chunked\nrun GET /a cap=100\n')
        self.assertEqual(results(out)[0]['err'], 'overflow')

    def test_cut_body_closes_and_get_retries_once(self):
        out = run('resp 200 100 cut=40\nresp 200 100\n'
                  'run GET /audio cap=1000\ncounts\n')
        r = results(out)[0]
        self.assertEqual((r['err'], r['retried'], r['first'], r['got']),
                         ('ok', '1', 'read', '100'))
        self.assertEqual(opens(out), ['fresh', 'fresh'])
        self.assertEqual(counts(out)['leftover'], '1')

    def test_read_error_closes(self):
        out = run('resp 200 100\nresp 200 100\nresp 200 5\nresp 200 5\n'
                  'run GET /a\nfail read\nrun GET /b\nrun GET /c\ncounts\n')
        # b は読みで失敗 → 閉じて 1 回送り直す (2 つめの応答を使う)
        self.assertEqual(opens(out), ['fresh', 'reused', 'fresh', 'reused'])
        self.assertEqual(results(out)[1]['retried'], '1')
        self.assertEqual(results(out)[1]['io'], '1')     # 本文の途中の失敗

    def test_aborted_long_poll_closes(self):
        out = run('resp 200 10\nresp 200 10\n'
                  'run GET /a\nabortafteropen\nrun GET /inbox?wait=25 long\n'
                  'abort 0\nrun GET /b\ncounts\n')
        rs = results(out)
        self.assertEqual(rs[1]['err'], 'aborted')
        self.assertEqual(rs[1]['retried'], '0')
        # 打ち切ったロングポーリングの接続は捨て、次は張り直す
        self.assertEqual(opens(out), ['fresh', 'reused', 'fresh'])


class RetryPolicyTest(unittest.TestCase):
    def test_post_is_never_resent(self):
        for stage in ('send', 'fetch', 'read'):
            with self.subTest(stage=stage):
                out = run('resp 202 10\nresp 202 10\n'
                          f'fail {stage}\nrun POST /talk body=10\ncounts\n')
                r = results(out)[0]
                self.assertEqual((r['err'], r['retried']), (stage, '0'))
                self.assertEqual(opens(out), ['fresh'])
                self.assertEqual(counts(out)['conn'], '0')   # 閉じてある

    def test_post_on_silently_dead_connection_is_not_resent(self):
        # 点検で見つからない死に方 (30 秒以内)。POST は送り直さずに失敗を返す。
        out = run('resp 200 10\nresp 202 10\n'
                  'run GET /a\nt 1000\nsilentdead\nrun POST /talk body=10\ncounts\n')
        r = results(out)[1]
        self.assertEqual((r['err'], r['retried'], r['reused']), ('timeout', '0', '1'))
        self.assertEqual(counts(out)['conn'], '0')

    def test_get_resend_once_then_give_up(self):
        out = run('resp 200 10\nresp 200 10\nresp 200 10\n'
                  'run GET /a\nfail open\nrun GET /b\n'
                  'fail send\nrun GET /c body=0\ncounts\n')
        rs = results(out)
        self.assertEqual((rs[1]['err'], rs[1]['retried'], rs[1]['first']),
                         ('ok', '1', 'send'))
        # 使い回した接続の送信の失敗は「相手が閉じていた」扱い (自己診断しない)
        self.assertEqual(rs[1]['io'], '0')

    def test_get_twice_failing_reports_failure(self):
        out = run('resp 200 10\nresp 200 10\n'
                  'run GET /a\nfail fetch\nrun GET /b\n')
        # 1 回目 fetch 失敗 → 送り直し (応答 2 つめ) で成功
        self.assertEqual(results(out)[1]['err'], 'ok')
        out = run('fail fetch\nrun GET /b\ncounts\n')
        r = results(out)[0]
        # 応答が無い = 2 回目も fetch で失敗 → 送り直しは 1 回まで
        self.assertEqual((r['err'], r['retried']), ('fetch', '1'))
        self.assertEqual(counts(out)['retries'], '1')
        self.assertEqual(opens(out), ['fresh', 'fresh'])

    def test_no_resend_on_connect_timeout_overflow(self):
        for script, err in (('fail connect\nrun GET /a\n', 'connect'),
                            ('resp 200 10\nfail timeout\nrun GET /a\n', 'timeout'),
                            ('resp 200 500\nrun GET /a cap=10\n', 'overflow')):
            with self.subTest(err=err):
                r = results(run(script + 'counts\n'))[0]
                self.assertEqual((r['err'], r['retried']), (err, '0'))

    def test_fresh_connection_io_failure_is_reported(self):
        # 張ったばかりの接続で書けない = 暗号の自己診断の対象 (README §20)
        out = run('resp 200 10\nfail send\nrun POST /talk body=10\n')
        self.assertEqual(results(out)[0]['io'], '1')

    def test_connect_failure_is_not_io(self):
        out = run('fail connect\nrun GET /a\n')
        self.assertEqual(results(out)[0]['io'], '0')


class AbortTest(unittest.TestCase):
    def test_abort_before_send_keeps_connection(self):
        out = run('resp 200 10\nresp 200 10\n'
                  'run GET /a\nabort 1\nrun GET /inbox?wait=25 long\n'
                  'abort 0\nrun GET /b\ncounts\n')
        rs = results(out)
        self.assertEqual(rs[1]['err'], 'aborted')
        self.assertEqual(opens(out), ['fresh', 'reused'])   # 何も送らず、残した
        self.assertEqual(counts(out)['aborted'], '1')

    def test_short_request_finishes_despite_abort(self):
        # 短い要求は送ったあとの打ち切りに従わない (最後まで読んで接続を残す)
        out = run('resp 200 10\nresp 200 10\n'
                  'abortafteropen\nrun GET /inbox\nabort 0\nrun GET /b\ncounts\n')
        rs = results(out)
        self.assertEqual((rs[0]['err'], rs[0]['kept']), ('ok', '1'))
        self.assertEqual(opens(out), ['fresh', 'reused'])

    def test_drop(self):
        out = run('resp 200 10\nresp 200 10\nrun GET /a\ndrop\nrun GET /b\n')
        self.assertEqual(opens(out), ['fresh', 'fresh'])


if __name__ == '__main__':
    unittest.main()
