"""Clips: POST / DELETE /clips (local only), GET /clips and its audio, stackee-clip.

Fake synthesis only: nothing here speaks.
"""
import http.client
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest
from unittest.mock import patch

import stackee_server as s

PCM = b'\0\1' * 1600   # what the fake engine answers for any sentence: 0.1 s
JSON = {'Content-Type': 'application/json'}


class ClipStoreTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name) / 'clips'

    def tearDown(self):
        self.tmp.cleanup()

    def listing(self, clips):
        return json.loads(clips.listing())

    def test_put_replace_delete_and_files(self):
        clips = s.Clips(self.dir)
        self.assertEqual(self.listing(clips), {'rev': 0, 'clips': []})
        clips.put('a', 'あ。', PCM, b'0\t\xe3\x81\x82\xe3\x80\x82\n')
        self.assertEqual((self.dir / 'a.pcm').read_bytes(), PCM)
        stored = json.loads((self.dir / 'a.json').read_text(encoding='utf-8'))
        self.assertEqual((stored['id'], stored['reply'], stored['subtitles'], stored['audio_bytes']),
                         ('a', 'あ。', '0\tあ。\n', len(PCM)))
        entry = self.listing(clips)['clips'][0]
        self.assertEqual(set(entry), {'id', 'created', 'audio_bytes', 'sample_rate', 'channels',
                                      'sample_width', 'reply', 'subtitles'})
        self.assertEqual((entry['sample_rate'], entry['channels'], entry['sample_width']), (16000, 1, 2))
        with patch('time.time', return_value=entry['created'] + 60):
            clips.put('a', 'い。', b'', b'0\ti\n')
        self.assertEqual(self.listing(clips)['clips'][0]['created'], entry['created'] + 60)
        self.assertEqual((self.dir / 'a.pcm').read_bytes(), b'')
        self.assertIsNone(clips.audio('a'))
        self.assertEqual(clips.summary()['count'], 1)
        self.assertTrue(clips.delete('a'))
        self.assertFalse(clips.delete('a'))
        self.assertEqual(sorted(p.name for p in self.dir.iterdir()), ['.rev'])
        self.assertEqual(self.listing(clips)['rev'], 3)

    def test_oldest_first_and_limits_delete_the_oldest(self):
        clips = s.Clips(self.dir, max_count=3)
        for n, name in enumerate(('c', 'a', 'b', 'd')):
            with patch('time.time', return_value=1000 + n // 2):   # ties are ordered by rev
                clips.put(name, name, PCM, b'')
        self.assertEqual([c['id'] for c in self.listing(clips)['clips']], ['a', 'b', 'd'])
        self.assertFalse((self.dir / 'c.pcm').exists())
        # Replacing moves a clip to the newest place.
        with patch('time.time', return_value=2000):
            clips.put('a', 'a2', PCM, b'')
        self.assertEqual([c['id'] for c in self.listing(clips)['clips']], ['b', 'd', 'a'])
        # By size: each clip is its PCM plus its JSON; the one just stored is always kept.
        small = s.Clips(Path(self.tmp.name) / 'small', max_bytes=2 * len(PCM) + 400)
        for name in ('x', 'y', 'z'):
            small.put(name, name, PCM, b'')
        self.assertEqual([c['id'] for c in self.listing(small)['clips']], ['y', 'z'])
        big = PCM * 10
        small.put('huge', 'huge', big, b'')
        self.assertEqual([c['id'] for c in self.listing(small)['clips']], ['huge'])

    def test_rev_never_goes_back_across_restarts(self):
        clips = s.Clips(self.dir)
        clips.put('a', 'a', PCM, b'')
        clips.put('b', 'b', PCM, b'')
        clips.delete('a')
        self.assertEqual(clips.summary()['rev'], 3)
        again = s.Clips(self.dir)
        self.assertEqual(again.summary()['rev'], 3)
        self.assertEqual([c['id'] for c in self.listing(again)['clips']], ['b'])
        self.assertEqual(again.audio('b'), PCM)
        again.put('c', 'c', PCM, b'')
        self.assertEqual(again.summary()['rev'], 4)
        # A lost .rev falls back to the highest rev a clip carries, then goes on from there.
        (self.dir / '.rev').unlink()
        third = s.Clips(self.dir)
        self.assertEqual(third.summary()['rev'], 4)
        third.delete('c')
        self.assertEqual(s.Clips(self.dir).summary()['rev'], 5)
        # Lowering the limits prunes on start, and that counts as a change too.
        for name in ('d', 'e'):
            third.put(name, name, PCM, b'')
        fourth = s.Clips(self.dir, max_count=1)
        self.assertEqual([c['id'] for c in self.listing(fourth)['clips']], ['e'])
        self.assertEqual(fourth.summary()['rev'], 9)

    def test_broken_files_and_leftovers_are_skipped(self):
        clips = s.Clips(self.dir)
        clips.put('ok', 'ok', PCM, b'')
        clips.put('short', 'short', PCM, b'')
        (self.dir / 'short.pcm').write_bytes(PCM[:10])
        (self.dir / 'bad.json').write_text('{', encoding='utf-8')
        (self.dir / '.ok.abc').write_bytes(b'x')
        (self.dir / 'ok.json.tmp').write_bytes(b'x')
        again = s.Clips(self.dir)
        self.assertEqual([c['id'] for c in self.listing(again)['clips']], ['ok'])
        self.assertFalse((self.dir / '.ok.abc').exists())
        self.assertFalse((self.dir / 'ok.json.tmp').exists())

    def test_listing_fits_64_kib_leaving_out_the_oldest(self):
        clips = s.Clips(self.dir)
        subtitles = ''.join('%d\t%s\n' % (n * 100, 'あ' * 15) for n in range(48)).encode()
        for n in range(12):
            with patch('time.time', return_value=1000 + n):
                clips.put('c%02d' % n, 'い' * 1300, PCM, subtitles)
        body = clips.listing()
        self.assertLessEqual(len(body), s.MAX_CLIPS_LIST)
        shown = [c['id'] for c in json.loads(body)['clips']]
        self.assertLess(len(shown), 12)
        self.assertEqual(shown, ['c%02d' % n for n in range(12 - len(shown), 12)])
        self.assertEqual(json.loads(body)['rev'], 12)


class ClipHttpTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name).resolve()
        self.pipeline = s.Pipeline('unused', echo=True)
        self.spoken = []
        def synthesize(text, root):
            self.spoken.append(text)
            return PCM
        self.pipeline.synthesize = synthesize
        self.clips = s.Clips(self.root / 'clips', max_count=3)
        self.server = s.ThreadingHTTPServer(('127.0.0.1', 0), s.Handler)
        self.server.jobs = s.Jobs(self.pipeline, say_url=s.say_url(self.server.server_address),
                                  clips=self.clips)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.pipeline.close()
        self.tmp.cleanup()

    def request(self, method, path, body=None, headers=None, timeout=5):
        conn = http.client.HTTPConnection(*self.server.server_address, timeout=timeout)
        if isinstance(body, (dict, list)):
            body = json.dumps(body, ensure_ascii=False).encode()
        conn.request(method, path, body, headers or {})
        resp = conn.getresponse()
        result = resp.status, dict(resp.getheaders()), resp.read()
        conn.close()
        return result

    def post(self, body):
        status, _, data = self.request('POST', '/clips', body, JSON)
        return status, json.loads(data)

    def listing(self):
        status, headers, data = self.request('GET', '/clips')
        self.assertEqual((status, headers['Content-Type']), (200, 'application/json'))
        return json.loads(data)

    def test_add_list_audio_replace_delete(self):
        status, body = self.post({'id': 'n-1', 'text': '一つ目です。[詳細](https://x.example)を見て。'})
        self.assertEqual((status, body), (200, {'id': 'n-1', 'audio_bytes': 2 * len(PCM) + len(s.SILENCE)}))
        self.assertEqual(self.spoken, ['一つ目です。', '詳細を見て。'])
        listing = self.listing()
        self.assertEqual(listing['rev'], 1)
        clip = listing['clips'][0]
        self.assertEqual((clip['id'], clip['reply'], clip['audio_bytes']),
                         ('n-1', '一つ目です。詳細を見て。', body['audio_bytes']))
        self.assertTrue(clip['subtitles'].startswith('0\t一つ目です。\n'))
        status, headers, audio = self.request('GET', '/clips/n-1/audio')
        self.assertEqual((status, headers['Content-Type'], len(audio)),
                         (200, 'application/octet-stream', body['audio_bytes']))
        # The same id replaces; voice=false keeps only captions.
        status, body = self.post({'id': 'n-1', 'text': '字幕だけ。', 'voice': False})
        self.assertEqual((status, body), (200, {'id': 'n-1', 'audio_bytes': 0}))
        listing = self.listing()
        self.assertEqual((listing['rev'], len(listing['clips'])), (2, 1))
        self.assertEqual((listing['clips'][0]['reply'], listing['clips'][0]['subtitles']),
                         ('字幕だけ。', '0\t字幕だけ。\n'))
        self.assertEqual(self.request('GET', '/clips/n-1/audio')[0], 404)
        status, _, data = self.request('DELETE', '/clips/n-1')
        self.assertEqual((status, json.loads(data)), (200, {'id': 'n-1', 'deleted': True}))
        self.assertEqual(self.request('DELETE', '/clips/n-1')[0], 404)
        self.assertEqual(self.listing(), {'rev': 3, 'clips': []})
        # A clip never reaches the inbox.
        self.assertEqual(self.server.jobs.inbox.summary(), {'count': 0, 'seq': 0})

    def test_an_id_is_made_when_omitted_and_old_ones_go_first(self):
        ids = []
        for n in range(4):
            status, body = self.post({'text': '%d 番。' % n})
            self.assertEqual(status, 200)
            self.assertRegex(body['id'], r'^[A-Za-z0-9_-]{1,40}$')
            ids.append(body['id'])
        self.assertEqual(len(set(ids)), 4)
        self.assertEqual([c['id'] for c in self.listing()['clips']], ids[1:])
        self.assertEqual(self.request('GET', '/clips/%s/audio' % ids[0])[0], 404)

    def test_only_this_machine_may_add_or_delete(self):
        self.assertEqual(self.post({'id': 'a', 'text': 'あ。'})[0], 200)
        with patch.object(s.Handler, 'local_client', return_value=False):
            self.assertEqual(self.post({'id': 'b', 'text': 'い。'}),
                             (403, {'error': 'local_only'}))
            self.assertEqual(self.request('DELETE', '/clips/a')[0], 403)
            # Reading is for anyone (the device reads through the gateway).
            self.assertEqual(len(self.listing()['clips']), 1)
            self.assertEqual(self.request('GET', '/clips/a/audio')[0], 200)
        self.assertEqual(self.request('DELETE', '/clips/a', headers={'Origin': 'http://127.0.0.1'})[0], 403)
        self.assertEqual([c['id'] for c in self.listing()['clips']], ['a'])

    def test_bad_requests_store_nothing(self):
        for body, headers, expected in (
                ({'text': 'x'}, {'Content-Type': 'text/plain'}, 415),
                ({'text': 'x'}, dict(JSON, Origin='http://127.0.0.1'), 403),
                ({'text': ''}, JSON, 400), ({'text': '  '}, JSON, 400),
                ({'voice': True}, JSON, 400), ({'text': 'x', 'voice': 'no'}, JSON, 400),
                ([], JSON, 400), (b'{', JSON, 400),
                ({'text': 'あ' * 1366}, JSON, 413),
                ({'text': 'https://example.com'}, JSON, 400),
                ({'id': 'a/b', 'text': 'x'}, JSON, 400), ({'id': '', 'text': 'x'}, JSON, 400),
                ({'id': 'x' * 41, 'text': 'x'}, JSON, 400), ({'id': 3, 'text': 'x'}, JSON, 400),
                ({'id': '.rev', 'text': 'x'}, JSON, 400)):
            with self.subTest(body=body, headers=headers):
                self.assertEqual(self.request('POST', '/clips', body, headers)[0], expected)
        self.assertEqual(self.listing(), {'rev': 0, 'clips': []})
        self.assertEqual(self.post({'id': 'x' * 40, 'text': 'あ' * 1365, 'voice': False})[0], 200)
        for method, path in (('GET', '/clips/'), ('GET', '/clips?x=1'), ('GET', '/clips/a'),
                             ('GET', '/clips/a/audio?x=1'), ('GET', '/clips/%2E%2E/audio'),
                             ('DELETE', '/clips'), ('DELETE', '/clips/a/audio'),
                             ('DELETE', '/talk'), ('POST', '/clips/a')):
            with self.subTest(method=method, path=path):
                self.assertEqual(self.request(method, path)[0], 404)

    def test_a_failed_synthesis_is_500_and_stores_nothing(self):
        def refuse(text, root):
            raise RuntimeError('VOICEVOX is down')
        self.pipeline.synthesize = refuse
        self.assertEqual(self.post({'id': 'a', 'text': 'こんにちは。'}),
                         (500, {'error': 'VOICEVOX is down'}))
        self.assertEqual(self.listing(), {'rev': 0, 'clips': []})

    def test_clips_are_independent_of_the_job_slot(self):
        self.server.jobs.busy = True
        self.assertEqual(self.post({'id': 'a', 'text': 'あ。'})[0], 200)

    def test_without_a_store_the_routes_say_so(self):
        self.server.jobs.clips = None
        self.assertEqual(self.request('GET', '/clips')[0], 503)
        self.assertEqual(self.post({'text': 'あ。'})[0], 503)
        self.assertEqual(self.request('DELETE', '/clips/a')[0], 503)

    def test_stackee_clip_command(self):
        script = str(s.BIN_DIR / 'stackee-clip')
        env = dict(os.environ, STACKEE_CLIP_URL=self.server.jobs.say_url.rsplit('/', 1)[0] + '/clips')
        def run(*argv, stdin=b''):
            return subprocess.run([script] + list(argv), input=stdin, env=env,
                                  capture_output=True, timeout=10)
        done = run('add', '--id', 'mine', '一つ目。')
        self.assertEqual((done.returncode, done.stdout), (0, b'mine\n'), done.stderr)
        done = run('add', '--no-voice', stdin='標準入力から。'.encode())
        self.assertEqual(done.returncode, 0, done.stderr)
        made = done.stdout.decode().strip()
        listing = self.listing()
        self.assertEqual([c['id'] for c in listing['clips']], ['mine', made])
        self.assertEqual(listing['clips'][1]['audio_bytes'], 0)
        done = run('ls')
        lines = done.stdout.decode().splitlines()
        self.assertEqual(lines[0], 'rev 2, 2 clip(s)')
        self.assertTrue(lines[1].startswith('mine\t') and lines[1].endswith('\t0.1s\t一つ目。'))
        self.assertIn('\tno-voice\t標準入力から。', lines[2])
        self.assertEqual((run('rm', 'mine').returncode, run('rm', 'mine').returncode), (0, 1))
        self.assertEqual([c['id'] for c in self.listing()['clips']], [made])
        for argv, expected in ((['add', 'https://example.com'], 1), (['add', '--id', 'a b', 'x'], 1),
                               (['add'], 2), ([], 2)):
            failed = run(*argv, stdin=b'  ')
            self.assertEqual(failed.returncode, expected, failed.stderr)
            self.assertTrue(failed.stderr)
        unreachable = dict(env, STACKEE_CLIP_URL='http://127.0.0.1:9/clips')
        failed = subprocess.run([script, 'ls'], env=unreachable, capture_output=True, timeout=10)
        self.assertEqual(failed.returncode, 1)

    def test_key_commands_get_the_clip_url(self):
        self.assertTrue(self.server.jobs.say_url.endswith('/say'))
        with tempfile.TemporaryDirectory() as tmp:
            keys = s.KeySettings(tmp)
            self.server.jobs.keys = keys
            out = Path(tmp) / 'env'
            self.server.jobs.run_command('Custom_1', 'echo "$STACKEE_CLIP_URL" > "%s"' % out, 10)
            self.assertEqual(out.read_text().strip(),
                             self.server.jobs.say_url[:-len('/say')] + '/clips')

    def test_admin_state_shows_the_clips(self):
        self.assertEqual(self.post({'id': 'a', 'text': 'あ。'})[0], 200)
        summary = self.clips.summary()
        self.assertEqual((summary['count'], summary['rev'], summary['max_count']), (1, 1, 3))
        self.assertGreater(summary['bytes'], len(PCM))


if __name__ == '__main__':
    unittest.main()
