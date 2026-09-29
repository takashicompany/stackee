#!/usr/bin/env python3
"""クリップの同期と再生の順番 (main/stackee_clipsm.c) を Mac 上で確かめる。**実機に触らない。**

本物の状態機械を Mac 用にビルドし (ASan + UBSan)、偽の時計・通信・FAT を
つないで台本を流す (hostbuild/clip_main.c)。見ている約束 (README §17-2f):

  * 起動後、目録 (FAT の clips/) を読んでから 1 回、以後 5 分ごとに GET /clips
  * rev が前回「最後まで」同期できたものと同じなら何もしない
  * 一覧に無いローカルのクリップは消す。**一覧の取得に失敗したら何も消さない**
    (通信の失敗・404・500・形式の不正)
  * 無いものを新しい順に取って FAT に書く。字幕だけ (audio_bytes=0) は音声を取らない
  * 余白 512 KB を残せないなら、入れるものより古いものから消す。それでも
    入らないものは飛ばす (新しいものは消さない)
  * 書きかけを残さない (書き込みの失敗・打ち切りで目録に載らない)
  * キー (abort) / 暇でなくなった (can 0) で打ち切る。打ち切りは 1 分後に続きから
  * 再生の順番: まだ鳴らしていないものを新しい順 → 全部済んだら古い順に循環。0 件は -1

  python3 firmware/tools/test_clip_host.py
"""
import json
import os
import re
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
IDF = os.path.dirname(HERE)
SANITIZE = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']

_BINARY = None

MB = 1024 * 1024


def binary():
    global _BINARY
    if _BINARY is None:
        out = os.path.join(tempfile.mkdtemp(), 'clip')
        cmd = (['cc', '-O1', '-std=gnu11', '-Wall', '-Werror'] + SANITIZE +
               ['-I', os.path.join(IDF, 'main'), '-o', out,
                os.path.join(IDF, 'hostbuild/clip_main.c'),
                os.path.join(IDF, 'main/stackee_clipsm.c'),
                os.path.join(IDF, 'main/stackee_jsonlite.c')])
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


def clip(cid, created, audio=32000, **extra):
    obj = {'id': cid, 'created': created, 'audio_bytes': audio,
           'reply': '本文 %s' % cid, 'subtitles': '0\t%s\n' % cid}
    if audio:
        obj.update(sample_rate=16000, channels=1, sample_width=2)
    obj.update(extra)
    return obj


def listing(rev, clips):
    return 'resp 200 ' + json.dumps({'rev': rev, 'clips': clips},
                                    ensure_ascii=False) + '\n'


def pcm(n):
    return 'resp 200 PCM:%d\n' % n


def info(text):
    rows = re.findall(r'^CLIPINFO (.*)$', text, re.M)
    assert rows, text
    return parse_info(rows[-1])


def parse_info(body):
    """CLIPINFO の 1 行。error= は空白を含むので ids= の手前までを丸ごと取る。"""
    head, ids = body.split(' ids=')
    head, error = head.split(' error=')
    out = dict(kv.split('=', 1) for kv in head.split(' '))
    out['error'] = error
    out['ids'] = [x for x in ids.split(',') if x]
    return out


def https(text):
    return re.findall(r'^HTTP GET (\S+) (\d+)$', text, re.M)


def fs_jobs(text):
    return re.findall(r'^FS (\w+) (\S+) (\d+)$', text, re.M)


def files(text):
    return [(m[1], m[2], int(m[3])) for m in
            re.findall(r'^FILE (\d+) (\S+) (\S+) (\d+)$', text, re.M)]


class SyncTest(unittest.TestCase):
    def test_first_sync_downloads_newest_first_and_deletes_the_rest(self):
        out = run('have gone 50 3200\n' +
                  listing(3, [clip('a', 100), clip('b', 200), clip('c', 300)]) +
                  pcm(32000) * 3 + 't 500\nprint\nfiles\n')
        # 目録 → 一覧 → 無いもの (gone) を消す → c, b, a の順に取る。
        self.assertEqual(fs_jobs(out)[0][0], 'scan')
        self.assertEqual([p for p, _ in https(out)],
                         ['/clips', '/clips/c/audio', '/clips/b/audio', '/clips/a/audio'])
        self.assertEqual([j[:2] for j in fs_jobs(out)[1:]],
                         [('remove', 'gone'), ('write', 'c'), ('write', 'b'),
                          ('write', 'a')])
        # 音声の受け皿はちょうど audio_bytes (余計に取らない)。
        self.assertEqual(https(out)[1][1], '32000')
        i = info(out)
        self.assertEqual(i['result'], 'ok')
        self.assertEqual(i['rev_done'], '3')
        self.assertEqual(i['downloads'], '3')
        self.assertEqual(i['removed'], '1')
        self.assertEqual(sorted(i['ids']), ['a', 'b', 'c'])
        self.assertEqual(sorted(f[0] for f in files(out)), ['a', 'b', 'c'])

    def test_same_list_does_nothing(self):
        out = run(listing(3, [clip('a', 100)]) + pcm(32000) +
                  listing(3, [clip('a', 100)]) +
                  't 500\nt 300000\nprint\nfiles\n')
        i = info(out)
        self.assertEqual(i['result'], 'same')
        self.assertEqual(i['same'], '1')
        self.assertEqual(i['ids'], ['a'])
        self.assertEqual([p for p, _ in https(out)], ['/clips', '/clips/a/audio', '/clips'])
        self.assertEqual(len(fs_jobs(out)), 2)          # 目録と最初の書き込みだけ

    def test_same_rev_is_still_compared(self):
        """★ サーバーが rev を進めずに作り直しても取りこぼさない (一覧は手元にある)。"""
        out = run(listing(3, [clip('a', 100)]) + pcm(32000) +
                  listing(3, [clip('a', 200, audio=64000)]) + pcm(64000) +
                  't 500\nt 300000\nprint\nfiles\n')
        i = info(out)
        self.assertEqual(i['result'], 'ok')
        self.assertEqual(i['replaced'], '1')
        self.assertEqual(files(out), [('a', '200', 64000)])

    def test_sync_now_compares_even_when_the_rev_is_the_same(self):
        out = run(listing(3, [clip('a', 100)]) + pcm(32000) +
                  listing(3, [clip('b', 200)]) + pcm(32000) +
                  't 500\nsyncnow\nt 500\nprint\n')
        i = info(out)
        self.assertEqual(i['result'], 'ok')
        self.assertEqual(i['ids'], ['b'])

    def test_runs_again_every_five_minutes(self):
        out = run(listing(1, []) + listing(2, [clip('a', 100)]) + pcm(32000) +
                  't 500\nprint\nt 299000\nprint\nt 1000\nprint\n')
        rows = re.findall(r'^CLIPINFO .*syncs=(\d+).*$', out, re.M)
        self.assertEqual(rows, ['1', '1', '2'])
        self.assertEqual(info(out)['ids'], ['a'])

    def test_waits_until_it_may_run(self):
        out = run('can 0\n' + listing(1, [clip('a', 100)]) + pcm(32000) +
                  't 5000\nwants\nprint\ncan 1\nt 500\nprint\n')
        self.assertIn('WANTS 1', out)       # 受け箱の待ちは譲る
        rows = re.findall(r'^CLIPINFO .*syncs=(\d+).*$', out, re.M)
        self.assertEqual(rows, ['0', '1'])


class NothingDeletedOnFailureTest(unittest.TestCase):
    """★ 一覧の取得に失敗したら、ローカルのクリップは 1 つも消さない。"""

    def check(self, resp, result, sleep=False):
        out = run('have keep1 100 3200\nhave keep2 200 3200\n' + resp +
                  't 500\nprint\nfiles\n')
        i = info(out)
        self.assertEqual(sorted(i['ids']), ['keep1', 'keep2'], out)
        self.assertEqual(sorted(f[0] for f in files(out)), ['keep1', 'keep2'])
        self.assertNotIn('FS remove', out)
        self.assertEqual(i['result'], result)
        self.assertEqual(i['valid'], '0')       # rev_done は進めない
        return out, i

    def test_network_error(self):
        _, i = self.check('resperr\n', 'error')
        self.assertGreater(int(i['next_in']), 290000)     # 5 分後にまた

    def test_relay_without_clips_sleeps_ten_minutes(self):
        out, i = self.check('resp 404 {"error":"not found"}\n', '404')
        self.assertEqual(i['phase'], 'sleep')
        self.assertGreater(int(i['next_in']), 590000)
        # 休んでいる間は受け箱の待ちを譲らない。
        out = run('resp 404 {}\nt 500\ncan 0\nwants\nt 590000\nwants\n'
                  't 10000\nwants\n')
        self.assertEqual(re.findall(r'^WANTS (\d)$', out, re.M), ['0', '0', '1'])

    def test_server_error(self):
        self.check('resp 500 {"error":"boom"}\n', 'error')

    def test_broken_json(self):
        self.check('resp 200 {"rev":3,"clips":[{"id":"a"\n', 'error')

    def test_missing_rev(self):
        self.check('resp 200 {"clips":[]}\n', 'error')


class CapacityTest(unittest.TestCase):
    def test_evicts_the_oldest_to_keep_the_reserve(self):
        # 空き 1.2 MB。新しい 1 MB を入れると余白 512 KB を割るので、いちばん
        # 古いもの (old1) だけ消す。1 つ消せば入るので old2 は残る。
        out = run('have old2 20 400000\nhave old1 10 400000\n'
                  'fscluster 4096\nfsfree 1258291\n' +
                  listing(5, [clip('old1', 10, 400000), clip('old2', 20, 400000),
                              clip('new', 30, 1000000)]) +
                  pcm(1000000) + 't 500\nprint\nfiles\n')
        i = info(out)
        self.assertEqual(i['result'], 'ok')
        self.assertEqual(i['evicted'], '1')
        self.assertEqual([j[:2] for j in fs_jobs(out)[1:]],
                         [('remove', 'old1'), ('write', 'new')])
        self.assertEqual(sorted(i['ids']), ['new', 'old2'])
        free = int(re.search(r'^FREE (\d+)$', out, re.M).group(1))
        self.assertGreaterEqual(free, 512 * 1024)

    def test_does_not_evict_newer_clips_for_an_older_one(self):
        # 新しい 2 件で埋まっている。古い大きなものを取るために新しいものは消さない。
        out = run('have n1 300 400000\nhave n2 400 400000\n'
                  'fscluster 4096\nfsfree 700000\n' +
                  listing(5, [clip('big_old', 100, 600000),
                              clip('n1', 300, 400000), clip('n2', 400, 400000)]) +
                  't 500\nprint\nfiles\n')
        i = info(out)
        self.assertEqual(i['result'], 'ok')
        self.assertEqual(i['space'], '1')
        self.assertEqual(i['evicted'], '0')
        self.assertEqual(sorted(i['ids']), ['n1', 'n2'])
        self.assertNotIn('/clips/big_old/audio', out)

    def test_does_not_evict_when_it_would_not_fit_anyway(self):
        # 古いものを全部消しても入らない大きさなら、1 つも消さずに飛ばす。
        out = run('have o1 10 100000\nfscluster 4096\nfsfree 300000\n' +
                  listing(5, [clip('o1', 10, 100000), clip('huge', 50, 3000000)]) +
                  't 500\nprint\n')
        i = info(out)
        self.assertEqual(i['evicted'], '0')
        self.assertEqual(i['space'], '1')
        self.assertEqual(i['ids'], ['o1'])

    def test_need_rounds_up_to_clusters(self):
        # 2 KB クラスタで 1 B の音声 + 1 B のメタ = 4 KB。空き 516 KB ちょうどなら入る。
        out = run('fscluster 2048\nfsfree %d\n' % (512 * 1024 + 4096) +
                  listing(1, [clip('t', 1, 2)]) + pcm(2) + 't 500\nprint\n')
        self.assertEqual(info(out)['space'], '0')


class NoHalfWrittenClipTest(unittest.TestCase):
    def test_write_failure_leaves_nothing_and_retries_later(self):
        out = run('fsfail write\n' + listing(2, [clip('a', 100)]) + pcm(32000) +
                  't 500\nprint\nfiles\n' +
                  listing(2, [clip('a', 100)]) + pcm(32000) + 't 300000\nprint\nfiles\n')
        rows = re.findall(r'^CLIPINFO (.*)$', out, re.M)
        first = parse_info(rows[0])
        self.assertEqual(first['result'], 'error')
        self.assertEqual(first['count'], '0')
        self.assertEqual(first['fails'], '1')
        self.assertEqual(first['valid'], '0')
        # 次の周 (rev は同じでも「最後まで」行っていないので) 取り直して入る。
        i = info(out)
        self.assertEqual(i['result'], 'ok')
        self.assertEqual(i['ids'], ['a'])
        self.assertEqual([f[0] for f in files(out)], ['a'])

    def test_abort_while_writing_keeps_no_partial_clip(self):
        # 目録は既定の 5 ms、そのあとの書き込みを 200 ms にする。
        out = run(listing(2, [clip('a', 100), clip('b', 200)]) +
                  pcm(32000) * 2 + 't 3\nfsdelay 200\nt 100\nabort\nt 400\n'
                  'print\nfiles\n')
        self.assertIn('FSABORT', out)
        self.assertRegex(out, r'FSDONE write 4')     # ABORTED
        i = info(out)
        self.assertEqual(i['count'], '0')
        self.assertEqual(i['aborts'], '1')
        self.assertEqual(i['result'], 'aborted')
        self.assertEqual(files(out), [])
        # 打ち切ったら 1 分後に続きから。
        self.assertGreater(int(i['next_in']), 59000)
        self.assertLessEqual(int(i['next_in']), 60000)

    def test_abort_while_downloading_closes_the_request(self):
        out = run('respdelay 1000\n' + listing(2, [clip('a', 100)]) + pcm(32000) +
                  't 1200\nabort\nt 100\nprint\nfiles\n')
        self.assertIn('HTTP GET /clips/a/audio', out)
        self.assertIn('HABORT', out)
        self.assertNotIn('FS write', out)
        self.assertEqual(info(out)['count'], '0')

    def test_losing_the_idle_state_aborts(self):
        out = run('respdelay 1000\n' + listing(2, [clip('a', 100)]) + pcm(32000) +
                  't 1200\ncan 0\nt 100\nprint\nwants\n')
        self.assertIn('HABORT', out)
        i = info(out)
        self.assertEqual(i['result'], 'aborted')
        self.assertIn('WANTS 0', out)

    def test_abort_resumes_where_it_left_off(self):
        out = run(listing(2, [clip('a', 100), clip('b', 200)]) + pcm(32000) +
                  't 3\nfsdelay 50\nt 30\n' +          # b を書いている最中
                  'abort\nt 100\n' +
                  listing(2, [clip('a', 100), clip('b', 200)]) + pcm(32000) * 2 +
                  't 61000\nprint\n')
        self.assertIn('FSABORT', out)
        i = info(out)
        self.assertEqual(i['aborts'], '1')
        self.assertEqual(i['result'], 'ok')
        self.assertEqual(sorted(i['ids']), ['a', 'b'])


class ListContentTest(unittest.TestCase):
    def test_subtitle_only_clips_are_not_fetched(self):
        out = run(listing(1, [clip('s', 100, audio=0)]) + 't 500\nprint\nfiles\n')
        self.assertEqual([p for p, _ in https(out)], ['/clips'])
        self.assertIn(('write', 's', '1'), fs_jobs(out))
        self.assertEqual(files(out), [('s', '100', 0)])

    def test_a_missing_audio_skips_only_that_clip(self):
        out = run(listing(1, [clip('a', 100), clip('b', 200)]) +
                  'resp 404 {}\n' + pcm(32000) + 't 500\nprint\n')
        i = info(out)
        self.assertEqual(i['result'], 'ok')
        self.assertEqual(i['ids'], ['a'])

    def test_wrong_audio_length_fails(self):
        out = run(listing(1, [clip('a', 100)]) + pcm(31998) + 't 500\nprint\n')
        i = info(out)
        self.assertEqual(i['result'], 'error')
        self.assertEqual(i['count'], '0')
        self.assertIn('長さ', i['error'])

    def test_bad_entries_are_skipped(self):
        bad = [clip('ok1', 100),
               clip('has space', 110),                       # id が不正
               clip('x' * 41, 120),                          # 41 文字
               clip('wrongrate', 130, sample_rate=44100),    # 音声形式が違う
               clip('odd', 140, audio=33333),                # 奇数バイト
               clip('toobig', 150, audio=3840002)]           # 120 秒超
        out = run(listing(1, bad) + pcm(32000) + 't 500\nprint\n')
        i = info(out)
        self.assertEqual(i['ids'], ['ok1'])
        self.assertEqual(i['bad'], '5')
        self.assertEqual([p for p, _ in https(out)], ['/clips', '/clips/ok1/audio'])

    def test_keeps_a_local_copy_when_the_server_entry_is_malformed(self):
        out = run('have keep 100 32000\n' +
                  listing(2, [clip('keep', 100, sample_rate=8000)]) + 't 500\nprint\n')
        self.assertEqual(info(out)['ids'], ['keep'])
        self.assertNotIn('FS remove', out)

    def test_a_changed_clip_is_fetched_again(self):
        out = run('have a 100 32000\n' +
                  listing(2, [clip('a', 100, audio=64000)]) + pcm(64000) +
                  't 500\nprint\nfiles\n')
        # ★ 新しい版を書き終えてから古い版を消す (消してから取るのではない)。
        self.assertEqual([j[:2] for j in fs_jobs(out)[1:]],
                         [('write', 'a'), ('remove', 'a')])
        self.assertEqual(files(out), [('a', '100', 64000)])
        self.assertEqual(info(out)['replaced'], '1')

    def test_keeps_only_the_newest_64(self):
        many = [clip('c%03d' % n, 1000 + n, audio=0) for n in range(70)]
        out = run(listing(1, many) + 't 2000\nprint\n')
        i = info(out)
        self.assertEqual(i['count'], '64')
        self.assertNotIn('c005', i['ids'])
        self.assertIn('c006', i['ids'])
        self.assertIn('c069', i['ids'])

    def test_string_created_sorts_as_text(self):
        out = run(listing(1, [clip('x', '2026-09-30T10:00:00Z'),
                              clip('y', '2026-09-30T09:00:00Z')]) +
                  pcm(32000) * 2 + 't 500\nprint\n')
        self.assertEqual([p for p, _ in https(out)][1:],
                         ['/clips/x/audio', '/clips/y/audio'])

    def test_braces_inside_strings_do_not_confuse_the_list(self):
        c = clip('a', 100)
        c['reply'] = 'かっこ } { と "引用" と \\ を含む'
        c['subtitles'] = '0\t}{\n'
        out = run(listing(1, [c, clip('b', 200)]) + pcm(32000) * 2 +
                  't 500\nprint\n')
        self.assertEqual(sorted(info(out)['ids']), ['a', 'b'])

    def test_clear_removes_everything_and_forgets_the_rev(self):
        out = run(listing(1, [clip('a', 100)]) + pcm(32000) + 't 500\n'
                  'clear\nt 100\nprint\nfiles\n' +
                  listing(1, [clip('a', 100)]) + pcm(32000) + 't 300000\nprint\n')
        self.assertIn('CLEAR 1', out)
        rows = re.findall(r'^CLIPINFO .*count=(\d+).*valid=(\d).*result=(\S+)', out, re.M)
        self.assertEqual(rows[0], ('0', '0', 'ok'))
        self.assertEqual(rows[1][2], 'ok')          # 同じ rev でも取り直した
        self.assertEqual(info(out)['ids'], ['a'])



class ReplaceTest(unittest.TestCase):
    """同じ id のまま作り直されたクリップ (created / audio_bytes / 本文が変わる)。

    ★ 2026-09-30 実機: サーバーの tk-keyboard-packing が作り直されたのに、本体は
    古い音声・字幕のまま残った。新しい版を別の名前で書き終えてから古い版と入れ替え、
    途中で失敗・打ち切りなら古い版を残す。
    """

    OLD = clip('topic', 1790699514, audio=32000)
    NEW = clip('topic', 1790702237, audio=64000)

    def first(self):
        return listing(1, [self.OLD]) + pcm(32000) + 't 500\n'

    def test_new_created_replaces_and_is_unplayed_again(self):
        out = run(self.first() + 'can 0\nplay\ncan 1\nprint\n' +
                  listing(2, [self.NEW]) + pcm(64000) + 'syncnow\nt 500\nprint\nfiles\n')
        rows = re.findall(r'^CLIPINFO (.*)$', out, re.M)
        self.assertEqual(parse_info(rows[0])['ids'], ['topic*'])      # 鳴らした
        i = info(out)
        self.assertEqual(i['ids'], ['topic'])                        # 新しい版は未再生
        self.assertEqual(i['replaced'], '1')
        self.assertEqual(files(out), [('topic', '1790702237', 64000)])
        self.assertRegex(out, r'(?m)^LOAD ok topic \d+ 32000$')     # 最初は古い版を鳴らした

    def test_text_only_change_is_replaced(self):
        new = dict(self.OLD, reply='言い直した本文')
        out = run(self.first() + listing(2, [new]) + pcm(32000) +
                  'syncnow\nt 500\nprint\n')
        self.assertEqual(info(out)['replaced'], '1')

    def test_download_failure_keeps_the_old_version(self):
        out = run(self.first() + listing(2, [self.NEW]) + 'resperr\n' +
                  'syncnow\nt 500\nprint\nfiles\n')
        i = info(out)
        self.assertEqual(i['result'], 'error')
        self.assertEqual(i['ids'], ['topic'])
        self.assertEqual(files(out), [('topic', '1790699514', 32000)])
        self.assertIn('LOAD ok topic', run(self.first() + listing(2, [self.NEW]) +
                                            'resperr\nsyncnow\nt 500\ncan 0\nplay\n'))

    def test_write_failure_keeps_the_old_version(self):
        out = run(self.first() + listing(2, [self.NEW]) + pcm(64000) +
                  'fsfail write\nsyncnow\nt 500\nprint\nfiles\n')
        self.assertEqual(info(out)['result'], 'error')
        self.assertEqual(files(out), [('topic', '1790699514', 32000)])

    def test_abort_keeps_the_old_version_and_retries(self):
        out = run(self.first() + listing(2, [self.NEW]) + pcm(64000) +
                  'fsdelay 200\nsyncnow\nt 60\nabort\nt 400\nprint\nfiles\n' +
                  'fsdelay 5\n' + listing(2, [self.NEW]) + pcm(64000) +
                  't 61000\nprint\nfiles\n')
        rows = re.findall(r'^CLIPINFO (.*)$', out, re.M)
        self.assertEqual(parse_info(rows[0])['result'], 'aborted')
        self.assertEqual(parse_info(rows[0])['ids'], ['topic'])
        before = out[:out.index('FREE')]
        self.assertEqual(files(before), [('topic', '1790699514', 32000)])
        self.assertEqual(files(out[out.index('FREE') + 1:]),
                         [('topic', '1790702237', 64000)])

    def test_unchanged_clip_is_not_touched(self):
        out = run(self.first() + listing(2, [self.OLD, clip('b', 1790700000)]) +
                  pcm(32000) + 'syncnow\nt 500\nprint\n')
        self.assertNotIn('/clips/topic/audio', out[out.index('LOG [clips] 同期 ok'):])
        self.assertEqual([j[:2] for j in fs_jobs(out)], [('scan', '-'), ('write', 'topic'),
                                                         ('write', 'b')])
        self.assertEqual(info(out)['replaced'], '0')

    def test_old_version_is_not_evicted_to_make_room_for_the_new(self):
        # 空きは新しい版 1 つぶんしか無い。他に古いものが無いので入れ替えは見送る
        # (古い版を先に消すと、取れなかったときに鳴らせるものを失う)。
        out = run('fscluster 4096\nfsfree %d\n' % (600 * 1024) + self.first() +
                  listing(2, [dict(self.NEW, audio_bytes=200000)]) +
                  'syncnow\nt 500\nprint\nfiles\n')
        i = info(out)
        self.assertEqual((i['evicted'], i['space'], i['replaced']), ('0', '1', '0'))
        self.assertEqual(files(out), [('topic', '1790699514', 32000)])

class PlayOrderTest(unittest.TestCase):
    def picks(self, out):
        return re.findall(r'^LOAD ok (\S+) ', out, re.M)

    def test_nothing_to_play(self):
        out = run('t 20\npick\nplay\n')
        self.assertIn('PICK -1 -', out)
        self.assertIn('LOAD none', out)

    def test_unplayed_newest_first_then_oldest_in_turn(self):
        out = run('have a 100 3200\nhave b 300 3200\nhave c 200 3200\n'
                  'can 0\nt 20\n' + 'play\n' * 8)
        self.assertEqual(self.picks(out), ['b', 'c', 'a', 'a', 'c', 'b', 'a', 'c'])

    def test_a_new_clip_comes_first_then_the_cycle_restarts(self):
        out = run('have a 100 3200\nhave b 200 3200\ncan 0\nt 20\n'
                  'play\nplay\nplay\n' +                  # b a (全部済み) a
                  listing(2, [clip('a', 100, audio=3200), clip('b', 200, audio=3200),
                              clip('n', 150, audio=3200)]) + pcm(3200) +
                  'can 1\nsyncnow\nt 500\ncan 0\n' +
                  'play\nplay\nplay\n')                   # n (未再生) → 古い順の最初から
        self.assertEqual(self.picks(out), ['b', 'a', 'a', 'n', 'a', 'n'])

    def test_loading_aborts_a_running_sync(self):
        out = run('have a 100 3200\n' +
                  listing(2, [clip('a', 100, audio=3200), clip('b', 200)]) +
                  pcm(32000) + 't 3\nfsdelay 100\nt 50\nplay\nprint\n')
        self.assertIn('FSABORT', out)
        self.assertEqual(self.picks(out), ['a'])
        self.assertEqual(info(out)['result'], 'aborted')

    def test_a_broken_file_is_skipped_next_time(self):
        out = run('have a 100 3200\nhave b 200 3200\ncan 0\nt 20\n'
                  'fsfail load\nplay\nplay\n')
        self.assertIn('LOAD fail b', out)
        self.assertEqual(self.picks(out), ['a'])



class AutoTest(unittest.TestCase):
    """STK_CLIP_AUTO / clips.auto — 自動取得 (5 分ごとの同期) の入り切り。"""

    def test_off_means_no_periodic_sync(self):
        out = run('autoinit 0 0\n' + listing(1, [clip('a', 100)]) + pcm(32000) +
                  't 700000\nprint\nwants\n')
        self.assertEqual(https(out), [])
        i = info(out)
        self.assertEqual((i['syncs'], i['auto']), ('0', '0'))
        self.assertIn('WANTS 0', out)       # 受け箱の待ちも譲らない

    def test_manual_sync_still_works_when_off(self):
        out = run('autoinit 0 0\n' + listing(1, [clip('a', 100)]) + pcm(32000) +
                  't 100\nsyncnow\nt 500\nprint\nt 700000\nprint\n')
        rows = re.findall(r'^CLIPINFO .*syncs=(\d+).*$', out, re.M)
        self.assertEqual(rows, ['1', '1'])  # 手で頼んだ 1 回だけ
        self.assertEqual(info(out)['ids'], ['a'])

    def test_turning_off_aborts_a_running_sync_without_leftovers(self):
        out = run(listing(2, [clip('a', 100), clip('b', 200)]) + pcm(32000) * 2 +
                  't 3\nfsdelay 200\nt 100\nauto 0\nt 400\nprint\nfiles\n'
                  't 700000\nprint\n')
        self.assertIn('AUTO 1 0', out)
        self.assertIn('FSABORT', out)
        i = info(out)
        self.assertEqual((i['count'], i['result'], i['syncs']), ('0', 'aborted', '1'))
        self.assertEqual(files(out), [])

    def test_turning_on_syncs_at_the_next_idle(self):
        out = run('autoinit 0 0\n' + listing(1, [clip('a', 100)]) + pcm(32000) +
                  't 1000\nauto 1\nt 500\nprint\n')
        self.assertIn('AUTO 1 1', out)
        self.assertEqual(info(out)['ids'], ['a'])

    def test_saved_only_when_changed_and_after_typing_stops(self):
        out = run('can 0\nidle 500\nt 10\nauto 0\nt 100\nprint\n'
                  'idle 2500\nt 10\nprint\nauto 0\nt 10\nauto 1\nt 10\n')
        self.assertEqual(re.findall(r'^SAVE (\d)$', out, re.M), ['0', '1'])
        rows = re.findall(r'^CLIPINFO .*dirty=(\d).*$', out, re.M)
        self.assertEqual(rows, ['1', '0'])  # 打鍵中は書かず、静かになってから 1 回

    def test_forced_off_cannot_be_turned_on(self):
        out = run('autoinit 1 1\n' + listing(1, [clip('a', 100)]) +
                  't 700000\nauto 1\nt 10\nprint\n')
        self.assertIn('AUTO 0 0', out)
        self.assertEqual(https(out), [])
        self.assertNotIn('SAVE', out)
        i = info(out)
        self.assertEqual((i['auto'], i['forced']), ('1', '1'))

if __name__ == '__main__':
    unittest.main()
