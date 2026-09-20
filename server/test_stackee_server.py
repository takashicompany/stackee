import array
import http.client
import io
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import wave

import stackee_agent
import stackee_server as s
from test_stackee_agent import FAKE_CLAUDE


def wav(seconds=.5, rate=16000, value=1000):
    out = io.BytesIO()
    with wave.open(out, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(array.array('h', [value] * int(rate * seconds)).tobytes())
    return out.getvalue()


class ReplyTests(unittest.TestCase):
    def test_remove_links_preserving_spoken_content(self):
        cases = [
            ('[AP通信](https://apnews.com/article/123)によると、勝ちました。',
             'AP通信によると、勝ちました。'),
            ('[説明](https://example.org/a_(b) "タイトル")を見ました。', '説明を見ました。'),
            ('結果はこちら https://example.com/a?q=1&b=2。勝ちました。', '結果はこちら。勝ちました。'),
            ('答えです（https://example.com）。次の話です。', '答えです。次の話です。'),
            ('答えです (https://example.com/a_(b))。', '答えです。'),
            ('[資料][ref]による説明。\n[ref]: https://example.com/path', '資料による説明。'),
            ('参考 <HTTPS://EXAMPLE.COM> `www.example.jp/news` example.co.jp/a //example.com/a', '参考'),
            ('勝ちました。citeturn0search0', '勝ちました。'),
            ('値は3.14、版は1.2.3です。', '値は3.14、版は1.2.3です。'),
        ]
        for raw, expected in cases:
            with self.subTest(raw=raw):
                self.assertEqual(s.clean_reply(raw), expected)

    def test_url_only_has_speakable_fallback(self):
        for raw in ('https://example.com', '[https://example.com](https://example.com)', '<https://example.com>。'):
            reply = s.clean_reply(raw)
            self.assertTrue(reply)
            self.assertNotIn('example', reply)
            self.assertNotIn('http', reply)

    def test_pipeline_sanitizes_before_limit_synthesis_and_response(self):
        p = s.Pipeline('unused')
        raw = 'https://example.com/' + 'x' * 200 + '\n[資料](https://example.org)によると、晴れです。'
        p.run = lambda argv, cwd, input=None: '天気は？'
        with patch.object(p.agent, 'ask', return_value=raw), \
                patch.object(p, 'synthesize', return_value=b'\0\1' * 8000) as synth:
            result, audio = p(wav())
        expected = '資料によると、晴れです。'
        self.assertEqual(synth.call_args.args[0], expected)
        self.assertEqual(result['reply'], expected)


class AudioTests(unittest.TestCase):
    def test_pipeline_reports_separate_stage_durations(self):
        p = s.Pipeline('unused')
        now = [0.0]
        def stage(seconds, result):
            def call(*args, **kwargs):
                now[0] += seconds
                return result
            return call
        with patch.object(s.time, 'monotonic', side_effect=lambda: now[0]), \
                patch.object(p, 'run', side_effect=stage(2, 'test')), \
                patch.object(p.agent, 'ask', side_effect=stage(3, 'reply')), \
                patch.object(p, 'synthesize', side_effect=stage(1, b'\0\1' * 100)):
            result, _ = p(wav())
        self.assertEqual(result['timings'], {'prepare_ms': 0, 'stt_ms': 2000,
                         'codex_ms': 3000, 'tts_ms': 1000, 'total_ms': 6000})

    def test_speaker_peak_limits_without_clipping_or_boosting(self):
        loud = array.array('h', [-32768, -16000, 0, 16000, 32767]).tobytes()
        result = s.limit_speaker_peak(loud)
        self.assertEqual(len(result), len(loud))
        self.assertLessEqual(s.peak(result), 8191)
        self.assertEqual(s.limit_speaker_peak(b'\0\0' * 100), b'\0\0' * 100)
        quiet = array.array('h', [-100, 100]).tobytes()
        self.assertEqual(s.limit_speaker_peak(quiet), quiet)

    def test_format_and_truncation(self):
        self.assertEqual(len(s.read_audio(wav())), 16000)
        for data in (b'garbage', wav(rate=8000), wav(.1), wav()[:-10]):
            with self.assertRaises(ValueError):
                s.read_audio(data)

    def test_silence_never_starts_process(self):
        p = s.Pipeline('unused')
        result, audio = p(wav(value=0))
        self.assertEqual(result['state'], 'ignored')
        self.assertEqual(audio, b'')

    def test_pipeline_reuses_agent_and_passes_only_current_message(self):
        p = s.Pipeline('unused')
        p.run = lambda argv, cwd, input=None: 'こんにちは'
        agent = p.agent
        with patch.object(agent, 'ask', return_value='こんにちは。') as ask, \
                patch.object(p, 'synthesize', return_value=b'\0\1' * 8000):
            for _ in range(2):
                result, audio = p(wav())
                self.assertEqual(result['reply'], 'こんにちは。')
                self.assertEqual(len(audio), 16000)
                self.assertIs(p.agent, agent)
        self.assertEqual(ask.call_count, 2)
        self.assertEqual(ask.call_args.args, ('こんにちは',))

    def test_codex_empty_final_is_error(self):
        p = s.Pipeline('unused')
        p.run = lambda argv, cwd, input=None: 'こんにちは'
        with patch.object(p.agent, 'ask', return_value=''):
            with self.assertRaisesRegex(RuntimeError, 'no final message'):
                p(wav())

    def test_timeout_kills_subprocess(self):
        import sys
        p = s.Pipeline('unused', timeout=.05)
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(RuntimeError, 'timed out'):
                p.run([sys.executable, '-c', 'import time; time.sleep(10)'], tmp)
        self.assertIsNone(p.process)


class VoicevoxTests(unittest.TestCase):
    def setUp(self):
        self.calls = []
        self.audio = wav(value=20000)
        owner = self
        class Engine(s.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'[{"styles":[{"id":3}]}]')

            def do_POST(self):
                body = self.rfile.read(int(self.headers['Content-Length']))
                owner.calls.append((self.path, body))
                self.send_response(200)
                self.end_headers()
                if self.path.startswith('/audio_query?'):
                    self.wfile.write(b'{"outputSamplingRate":24000,"outputStereo":true}')
                else:
                    self.wfile.write(owner.audio)
        self.server = s.ThreadingHTTPServer(('127.0.0.1', 0), Engine)
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()
        self.pipeline = s.Pipeline('unused', tts='voicevox',
            voicevox_url='http://127.0.0.1:' + str(self.server.server_port))

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def test_japanese_query_format_and_peak(self):
        reply, audio, _ = self.pipeline.fit('こんにちは & 元気？', Path('/unused'))
        self.assertEqual(reply, 'こんにちは & 元気？')
        self.assertEqual(len(audio), 16000)
        self.assertLessEqual(s.peak(audio), 8191)
        query = s.urllib.parse.parse_qs(s.urllib.parse.urlsplit(self.calls[0][0]).query)
        self.assertEqual(query['text'], ['こんにちは & 元気？'])
        self.assertEqual(query['speaker'], ['3'])
        synthesis = json.loads(self.calls[1][1])
        self.assertEqual(synthesis['outputSamplingRate'], 16000)
        self.assertIs(synthesis['outputStereo'], False)

    def test_invalid_and_oversized_audio_rejected(self):
        for data in (wav(rate=24000), wav()[:-4], b'x' * (s.MAX_REPLY_BYTES + 4097)):
            self.audio = data
            with self.assertRaises((ValueError, RuntimeError)):
                self.pipeline.synthesize('test', Path('/unused'))

    def test_linux_check_does_not_require_say_and_validates_speaker(self):
        with tempfile.NamedTemporaryFile() as model:
            self.pipeline.model = model.name
            with patch.object(self.pipeline.agent, 'start'), \
                    patch.object(s.shutil, 'which', side_effect=lambda cmd: None if cmd == 'say' else cmd):
                self.pipeline.check()
                self.assertEqual(self.calls[0][0], '/initialize_speaker?speaker=3&skip_reinit=true')
                self.pipeline.speaker = 999999
                with self.assertRaisesRegex(RuntimeError, 'speaker not found'):
                    self.pipeline.check()


class HttpTests(unittest.TestCase):
    def setUp(self):
        self.release = threading.Event()
        self.calls = []
        def pipeline(data):
            self.calls.append(data)
            self.release.wait(3)
            return {'state': 'done', 'transcript': 'test', 'reply': 'reply',
                    'subtitles': '0\tこんにちは\n1200\tさようなら\n'.encode('utf-8')}, b'\0\1' * 100
        self.server = s.ThreadingHTTPServer(('127.0.0.1', 0), s.Handler)
        self.server.jobs = s.Jobs(pipeline)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.release.set()
        if self.server.jobs.worker:
            self.server.jobs.worker.join(3)
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()

    def request(self, method, path, body=None, headers=None):
        conn = http.client.HTTPConnection(*self.server.server_address, timeout=3)
        conn.request(method, path, body, headers or {})
        resp = conn.getresponse()
        result = resp.status, dict(resp.getheaders()), resp.read()
        conn.close()
        return result

    def test_full_job_and_busy(self):
        status, headers, body = self.request('POST', '/talk', wav(), {'Content-Type': 'audio/wav'})
        self.assertEqual(status, 202)
        path = json.loads(body)['status_url']
        self.assertEqual(headers['Location'], path)
        self.assertEqual(self.request('GET', path + '/audio')[0], 409)
        self.assertEqual(self.request('POST', '/talk', wav(), {'Content-Type': 'audio/wav'})[0], 409)
        self.release.set()
        self.server.jobs.worker.join(3)
        state = json.loads(self.request('GET', path)[2])
        self.assertEqual(state['state'], 'done')
        self.assertEqual(state['audio_bytes'], 200)
        status, headers, audio = self.request('GET', state['audio_url'])
        self.assertEqual(status, 200)
        self.assertEqual(audio, b'\0\1' * 100)
        self.assertEqual(len(self.calls), 1)

    def test_subtitles_endpoint_and_unchanged_done_fields(self):
        status, _, body = self.request('POST', '/talk', wav(), {'Content-Type': 'audio/wav'})
        self.assertEqual(status, 202)
        path = json.loads(body)['status_url']
        self.assertEqual(self.request('GET', path + '/subtitles')[0], 409)
        self.release.set()
        self.server.jobs.worker.join(3)
        state = json.loads(self.request('GET', path)[2])
        self.assertEqual(state['subtitles_url'], path + '/subtitles')
        self.assertEqual(set(state), {'state', 'transcript', 'reply', 'audio_url', 'audio_bytes',
                                      'sample_rate', 'channels', 'sample_width', 'subtitles_url'})
        self.assertEqual((state['audio_url'], state['audio_bytes'], state['sample_rate'],
                          state['channels'], state['sample_width']),
                         (path + '/audio', 200, 16000, 1, 2))
        status, headers, subtitles = self.request('GET', state['subtitles_url'])
        self.assertEqual(status, 200)
        self.assertEqual(headers['Content-Type'], 'text/plain; charset=utf-8')
        self.assertEqual(subtitles.decode('utf-8'), '0\tこんにちは\n1200\tさようなら\n')
        self.assertEqual(self.request('GET', '/jobs/' + '0' * 32 + '/subtitles')[0], 404)

    def test_subtitles_are_absent_for_ignored_failed_and_silent_jobs(self):
        def ignored(data):
            return {'state': 'ignored', 'reason': 'silence'}, b''
        def failed(data):
            raise RuntimeError('test failure')
        def wordless(data):
            return {'state': 'done', 'transcript': 'test', 'reply': 'reply'}, b'\0\1' * 10
        for worker, expected in ((ignored, 'ignored'), (failed, 'error'), (wordless, 'done')):
            with self.subTest(state=expected):
                self.server.jobs.pipeline = worker
                ident = self.server.jobs.submit(wav())
                self.server.jobs.worker.join(3)
                path = '/jobs/' + ident
                state = json.loads(self.request('GET', path)[2])
                self.assertEqual(state['state'], expected)
                self.assertNotIn('subtitles_url', state)
                self.assertEqual(self.request('GET', path + '/subtitles')[0], 404)

    def test_reject_bad_requests_without_work(self):
        cases = [('/wrong', wav(), {'Content-Type': 'audio/wav'}, 404),
                 ('/talk', wav(), {'Content-Type': 'text/plain'}, 415),
                 ('/talk', wav(rate=8000), {'Content-Type': 'audio/wav'}, 400),
                 ('/talk', wav(), {'Content-Type': 'audio/wav', 'Origin': 'https://example.com'}, 403),
                 ('/talk', b'', {'Content-Type': 'audio/wav', 'Content-Length': str(s.MAX_UPLOAD + 1)}, 413)]
        for path, body, headers, expected in cases:
            self.assertEqual(self.request('POST', path, body, headers)[0], expected)
        self.assertEqual(self.calls, [])

    def test_health_and_unknown_job(self):
        self.assertTrue(json.loads(self.request('GET', '/health')[2])['ok'])
        self.assertEqual(self.request('GET', '/jobs/' + '0' * 32)[0], 404)

    def test_worker_failure_and_expiration(self):
        def fail(data):
            raise RuntimeError('test failure')
        self.server.jobs.pipeline = fail
        ident = self.server.jobs.submit(wav())
        self.server.jobs.worker.join(3)
        item = self.server.jobs.get(ident)
        self.assertEqual(item['state'], 'error')
        self.assertFalse(self.server.jobs.busy)
        self.server.jobs.entries[ident]['created'] = time.monotonic() - 301
        self.assertIsNone(self.server.jobs.get(ident))


class AdminTests(unittest.TestCase):
    """The admin page and its API; /talk must keep behaving exactly as before."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name).resolve()
        (self.root / 'defaults').mkdir()
        (self.root / 'defaults/AGENTS.md').write_text('Codex defaults', encoding='utf-8')
        (self.root / 'defaults/CLAUDE.md').write_text('Claude defaults', encoding='utf-8')
        (self.root / 'defaults/agent.json').write_text(json.dumps(
            {'agent': 'claude', 'codex': {'model': 'test-model', 'effort': 'none'},
             'claude': {'model': 'test-claude', 'effort': 'low'}}), encoding='utf-8')
        import sys
        claude = self.root / 'fake-claude'
        claude.write_text('#!' + sys.executable + '\n' + FAKE_CLAUDE)
        claude.chmod(0o700)
        self.agent = stackee_agent.Agent(self.root, '/nonexistent/codex', str(claude))
        self.agent.start()

        class FakePipeline:
            agent = self.agent

            def __call__(inner, data):
                return {'state': 'done', 'transcript': 'test', 'reply': 'reply'}, b''

        self.server = s.ThreadingHTTPServer(('127.0.0.1', 0), s.Handler)
        self.server.jobs = s.Jobs(FakePipeline())
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.agent.close()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.tmp.cleanup()

    def request(self, method, path, body=None, headers=None):
        conn = http.client.HTTPConnection(*self.server.server_address, timeout=5)
        conn.request(method, path, body, headers or {})
        resp = conn.getresponse()
        result = resp.status, dict(resp.getheaders()), resp.read()
        conn.close()
        return result

    def admin(self, method, path, body=None, extra=None):
        headers = {'X-Stackee-Admin': '1'}
        headers.update(extra or {})
        return self.request(method, path, body, headers)

    def test_startup_restores_runtime_files_from_defaults(self):
        for name in ('AGENTS.md', 'CLAUDE.md', 'agent.json'):
            self.assertTrue((self.root / name).is_file(), name)
        self.assertEqual((self.root / 'CLAUDE.md').read_text(encoding='utf-8'), 'Claude defaults')

    def test_state_reports_config_instructions_status_and_choices(self):
        status, headers, body = self.request('GET', '/admin/api/state')
        self.assertEqual(status, 200)
        data = json.loads(body)
        self.assertEqual(data['config']['agent'], 'claude')
        self.assertEqual(data['instructions'], {'codex': 'Codex defaults', 'claude': 'Claude defaults'})
        self.assertEqual(data['status']['agent'], 'claude')
        self.assertTrue(data['status']['running'])
        self.assertIs(data['busy'], False)
        self.assertEqual(data['choices']['claude_effort'], list(stackee_agent.CLAUDE_EFFORTS))
        self.assertIn('none', data['choices']['codex_effort'])

    def test_admin_page_is_served_without_external_resources(self):
        status, headers, body = self.request('GET', '/admin')
        self.assertEqual(status, 200)
        self.assertTrue(headers['Content-Type'].startswith('text/html'))
        page = body.decode('utf-8')
        self.assertIn('/admin/api/state', page)
        self.assertNotIn('http://', page.replace('http://127.0.0.1', ''))
        self.assertNotIn('https://', page)

    def test_config_is_saved_but_not_applied_until_apply(self):
        config = {'agent': 'codex', 'codex': {'model': 'test-model', 'effort': 'high'},
                  'claude': {'model': 'test-claude', 'effort': 'max'}}
        status, _, body = self.admin('PUT', '/admin/api/config', json.dumps(config),
                                     {'Content-Type': 'application/json'})
        self.assertEqual(status, 200)
        self.assertEqual(json.loads((self.root / 'agent.json').read_text(encoding='utf-8')), config)
        self.assertEqual(self.agent.status()['agent'], 'claude')

    def test_config_validation_errors_do_not_touch_the_file(self):
        original = (self.root / 'agent.json').read_bytes()
        for bad in ('{', json.dumps({'agent': 'gemini'}),
                    json.dumps({'agent': 'claude', 'claude': {'model': 'x', 'effort': 'ultra'}}),
                    json.dumps({'agent': 'codex', 'codex': {'model': '  '}})):
            with self.subTest(bad=bad):
                status, _, body = self.admin('PUT', '/admin/api/config', bad,
                                             {'Content-Type': 'application/json'})
                self.assertEqual(status, 400)
                self.assertIn('error', json.loads(body))
        self.assertEqual((self.root / 'agent.json').read_bytes(), original)

    def test_each_backend_keeps_its_own_instructions(self):
        """The page saves both tabs; one backend's text must never overwrite the other's."""
        texts = {'codex': 'Codex の新しい指示\nです\n'.encode('utf-8'),
                 'claude': 'Claude の新しい指示\nです\n'.encode('utf-8')}
        files = {'codex': 'AGENTS.md', 'claude': 'CLAUDE.md'}
        for kind in ('codex', 'claude'):
            status, _, _ = self.admin('PUT', '/admin/api/instructions/' + kind, texts[kind],
                                      {'Content-Type': 'text/plain; charset=utf-8'})
            self.assertEqual(status, 200)
            self.assertEqual((self.root / files[kind]).read_bytes(), texts[kind])
        other = {'codex': 'claude', 'claude': 'codex'}
        for kind in ('codex', 'claude'):
            self.assertNotEqual((self.root / files[kind]).read_bytes(), texts[other[kind]])
        served = json.loads(self.request('GET', '/admin/api/state')[2])['instructions']
        self.assertEqual({k: v.encode('utf-8') for k, v in served.items()}, texts)

    def test_instructions_must_not_be_blank_or_oversized(self):
        original = (self.root / 'CLAUDE.md').read_bytes()
        text = 'あたらしい指示\n'.encode('utf-8')
        self.assertEqual(self.admin('PUT', '/admin/api/instructions/claude', b'  \n',
                                    {'Content-Type': 'text/plain; charset=utf-8'})[0], 400)
        self.assertEqual(self.admin('PUT', '/admin/api/instructions/other', text)[0], 404)
        self.assertEqual(self.admin('PUT', '/admin/api/instructions/claude',
                                    b'x' * (s.MAX_ADMIN_BYTES + 1))[0], 413)
        self.assertEqual((self.root / 'CLAUDE.md').read_bytes(), original)

    def test_apply_restarts_the_backend_and_reports_failures(self):
        self.admin('PUT', '/admin/api/instructions/claude', 'べつの指示'.encode('utf-8'),
                   {'Content-Type': 'text/plain; charset=utf-8'})
        status, _, body = self.admin('POST', '/admin/api/apply', '{}',
                                     {'Content-Type': 'application/json'})
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)['status']['agent'], 'claude')
        self.assertTrue(json.loads(body)['status']['running'])
        (self.root / 'agent.json').write_text(json.dumps({'agent': 'claude', 'claude': {'model': ''}}))
        status, _, body = self.admin('POST', '/admin/api/apply', '{}',
                                     {'Content-Type': 'application/json'})
        self.assertEqual(status, 500)
        self.assertIn('error', json.loads(body))
        self.assertFalse(self.agent.status()['running'])

    def test_writes_require_the_admin_header_and_a_matching_origin(self):
        body = json.dumps({'agent': 'claude'})
        self.assertEqual(self.request('PUT', '/admin/api/config', body)[0], 403)
        self.assertEqual(self.request('POST', '/admin/api/apply', '{}')[0], 403)
        host = '%s:%s' % self.server.server_address
        self.assertEqual(self.admin('PUT', '/admin/api/config', body,
                                    {'Origin': 'http://evil.example'})[0], 403)
        self.assertEqual(self.admin('PUT', '/admin/api/config', body,
                                    {'Origin': 'http://' + host})[0], 200)
        self.assertEqual(self.request('OPTIONS', '/admin/api/config')[0], 501)

    def test_reset_forgets_one_conversation_and_keeps_the_other(self):
        state_file = self.root / '.state/session.json'
        stackee_agent.save_conversation(state_file, 'codex', 'thread-keep')
        stackee_agent.save_conversation(state_file, 'claude', 'session-drop')
        status, _, body = self.admin('POST', '/admin/api/conversation/claude/reset', '{}',
                                     {'Content-Type': 'application/json'})
        self.assertEqual(status, 200)
        data = json.loads(body)
        self.assertEqual(data['reset'], 'claude')
        self.assertIsNone(data['status']['conversations']['claude'])
        self.assertEqual(data['status']['conversations']['codex'], 'thread-keep')
        self.assertEqual(json.loads(state_file.read_text(encoding='utf-8')),
                         {'codex': {'thread_id': 'thread-keep'}})
        self.assertEqual(self.admin('POST', '/admin/api/conversation/gemini/reset', '{}')[0], 404)
        self.assertEqual(self.request('POST', '/admin/api/conversation/codex/reset', '{}')[0], 403)
        self.assertEqual(self.admin('POST', '/admin/api/conversation/codex/reset', '{}',
                                    {'Origin': 'http://evil.example'})[0], 403)
        self.assertEqual(json.loads(state_file.read_text(encoding='utf-8')),
                         {'codex': {'thread_id': 'thread-keep'}})

    def test_talk_and_health_are_unchanged(self):
        self.assertEqual(self.request('POST', '/talk', wav(), {
            'Content-Type': 'audio/wav', 'Origin': 'http://evil.example'})[0], 403)
        self.assertEqual(self.request('PUT', '/talk', wav(), {'Content-Type': 'audio/wav'})[0], 404)
        status, headers, body = self.request('POST', '/talk', wav(), {'Content-Type': 'audio/wav'})
        self.assertEqual(status, 202)
        self.server.jobs.worker.join(5)
        self.assertEqual(json.loads(self.request('GET', json.loads(body)['status_url'])[2])['state'], 'done')
        health = json.loads(self.request('GET', '/health')[2])
        self.assertEqual((health['ok'], health['agent']), (True, 'claude'))
        self.assertEqual(health['conversation']['cwd'], str(self.root))


class LengthTests(unittest.TestCase):
    """One synthesis per sentence, joined with a breath, stopped before the buffer overflows."""

    def pipeline(self, bytes_per_char=20000, refuse_over=None):
        p = s.Pipeline('unused')
        calls = []
        def synthesize(text, root):
            calls.append(text)
            if refuse_over is not None and len(text) > refuse_over:
                raise RuntimeError('VOICEVOX response exceeds device limit')
            return b'\0\1' * (len(text) * bytes_per_char // 2)
        p.synthesize = synthesize
        return p, calls

    def test_a_short_reply_is_a_single_synthesis(self):
        p, calls = self.pipeline()
        reply, audio, _ = p.fit('みじかい返答です。', Path('/unused'))
        self.assertEqual(reply, 'みじかい返答です。')
        self.assertEqual(calls, ['みじかい返答です。'])
        self.assertEqual(len(audio), 9 * 20000)
        self.assertNotIn(s.SILENCE, audio)

    def test_each_sentence_is_synthesised_on_its_own_and_joined_with_silence(self):
        p, calls = self.pipeline()
        text = 'ひとつ目です。ふたつ目です！みっつ目ですか？'
        reply, audio, _ = p.fit(text, Path('/unused'))
        self.assertEqual(reply, text)
        self.assertEqual(calls, ['ひとつ目です。', 'ふたつ目です！', 'みっつ目ですか？'])
        self.assertEqual(len(audio), len(text) * 20000 + 2 * len(s.SILENCE))
        self.assertEqual(len(s.SILENCE), 16000 * 2 * 150 // 1000)

    def test_the_total_stops_before_the_limit_without_cutting_a_sentence(self):
        p, calls = self.pipeline()
        text = ''.join('これは' + str(i) + '番目の文です。' for i in range(40))
        reply, audio, _ = p.fit(text, Path('/unused'))
        self.assertLessEqual(len(audio), s.MAX_REPLY_BYTES)
        self.assertTrue(text.startswith(reply), reply)
        self.assertTrue(reply.endswith('。'), reply)
        self.assertLess(len(reply), len(text))
        # Every sentence it accepted was synthesised once, and it stopped at the first miss.
        self.assertEqual(calls[:len(s.split_parts(reply, s.SENTENCE_MARKS))],
                         s.split_parts(reply, s.SENTENCE_MARKS))
        nxt = text[len(reply):]
        self.assertEqual(calls[-1], s.split_parts(nxt, s.SENTENCE_MARKS)[0])

    def test_a_first_sentence_that_is_too_long_falls_back_to_clauses(self):
        p, calls = self.pipeline()
        text = '、'.join('とても長い句' + str(i) for i in range(50)) + '。'
        reply, audio, _ = p.fit(text, Path('/unused'))
        self.assertLessEqual(len(audio), s.MAX_REPLY_BYTES)
        self.assertTrue(text.startswith(reply), reply)
        self.assertFalse(reply.endswith('、'), reply)
        self.assertGreater(len(reply), 0)
        self.assertEqual(calls[0], text)
        self.assertIn('、', calls[1])

    def test_a_sentence_the_engine_refuses_is_retried_clause_by_clause(self):
        p, calls = self.pipeline(bytes_per_char=100, refuse_over=30)
        text = 'みじかい文です。' + 'あ、' * 30 + 'おわり。'
        reply, audio, _ = p.fit(text, Path('/unused'))
        self.assertTrue(calls[0] == 'みじかい文です。')
        self.assertIn('あ、' * 30, calls[1])
        self.assertTrue(all(len(c) <= 30 for c in calls[2:]), calls[2:])
        self.assertIn('みじかい文です。', reply)
        self.assertIn('おわり。', reply)
        self.assertLessEqual(len(audio), s.MAX_REPLY_BYTES)

    def test_a_single_unbreakable_sentence_is_an_error(self):
        p, calls = self.pipeline()
        with self.assertRaisesRegex(RuntimeError, 'exceeds device limit'):
            p.fit('あ' * 300, Path('/unused'))

    def test_a_synthesis_that_always_fails_reports_its_own_error(self):
        p, calls = self.pipeline(refuse_over=0)
        with self.assertRaisesRegex(RuntimeError, 'VOICEVOX response exceeds device limit'):
            p.fit('ひとつ目です。ふたつ目です。', Path('/unused'))

    def test_the_limit_is_what_decides(self):
        p, calls = self.pipeline()
        text = ''.join('これは' + str(i) + '番目の文です。' for i in range(10))
        self.assertEqual(p.fit(text, Path('/unused'))[0], text)
        tight, audio, _ = p.fit(text, Path('/unused'), s.RATE * 2 * 30)
        self.assertLess(len(tight), len(text))
        self.assertLessEqual(len(audio), s.RATE * 2 * 30)
        self.assertTrue(tight.endswith('。'), tight)

    def test_the_pipeline_no_longer_cuts_the_reply_at_a_fixed_length(self):
        p = s.Pipeline('unused')
        p.run = lambda argv, cwd, input=None: 'test'
        long_reply = 'あ' * 300 + '。'
        with patch.object(p.agent, 'ask', return_value=long_reply), \
                patch.object(p, 'synthesize', return_value=b'\0\1' * 8000):
            result, audio = p(wav())
        self.assertEqual(result['reply'], long_reply)

    def test_split_parts_rejoins_into_the_original(self):
        text = 'ひとつ目です。ふたつ目です！みっつ目ですか？しめの文'
        pieces = s.split_parts(text, s.SENTENCE_MARKS)
        self.assertEqual(len(pieces), 4)
        self.assertEqual(''.join(pieces), text)


class SubtitleTests(unittest.TestCase):
    """Pages of at most 15 columns, timed from the PCM the server has already joined."""

    def pipeline(self, bytes_per_char=20000):
        p = s.Pipeline('unused')
        p.synthesize = lambda text, root: b'\0\1' * (len(text) * bytes_per_char // 2)
        return p

    @staticmethod
    def lines(body):
        return [line.split('\t') for line in body.decode('utf-8').splitlines()]

    def test_sentence_start_comes_from_the_joined_pcm(self):
        p = self.pipeline()
        reply, audio, body = p.fit('ひとつ目です。ふたつ目です。', Path('/unused'))
        # 7 characters x 20000 bytes = 140000 bytes = 4375 ms, then 150 ms of silence.
        self.assertEqual(len(audio), 7 * 20000 * 2 + len(s.SILENCE))
        self.assertEqual(self.lines(body), [['0', 'ひとつ目です。'], ['4525', 'ふたつ目です。']])
        self.assertEqual(4525, (7 * 20000 + len(s.SILENCE)) // (s.RATE * 2 // 1000))

    def test_pages_inside_a_sentence_share_its_duration_by_characters(self):
        p = self.pipeline()
        text = '春はあたたかいです、夏はあついです。'
        reply, audio, body = p.fit(text, Path('/unused'))
        self.assertEqual(reply, text)
        # 18 characters = 11250 ms; the second page starts after 10 of them.
        self.assertEqual(self.lines(body), [['0', '春はあたたかいです、'], ['6250', '夏はあついです。']])
        self.assertEqual(6250, round(len(text) * 20000 / 32 * 10 / len(text)))

    def test_the_say_backend_times_its_pages_the_same_way(self):
        p = s.Pipeline('unused', tts='say')
        def run(argv, cwd, input=None):
            Path(argv[argv.index('-o') + 1]).write_bytes(wav(seconds=len(argv[-1]) * .5))
            return ''
        p.run = run
        with tempfile.TemporaryDirectory() as tmp:
            reply, audio, body = p.fit('ひとつ目です。ふたつ目です。', Path(tmp))
        self.assertEqual(len(audio), 7 * 16000 * 2 + len(s.SILENCE))
        self.assertEqual(self.lines(body), [['0', 'ひとつ目です。'], ['3650', 'ふたつ目です。']])

    def test_a_long_clause_is_split_evenly_without_a_stub_page(self):
        # 40 columns need three pages, so they come out 13 + 13 + 14, not 15 + 15 + 10.
        self.assertEqual(s.split_columns('あ' * 40), ['あ' * 13, 'あ' * 13, 'あ' * 14])
        self.assertEqual(s.split_columns('北の地域では雪が多く降る季節です。'),
                         ['北の地域では雪が', '多く降る季節です。'])
        self.assertEqual(s.split_columns('あ' * 15), ['あ' * 15])
        for width in range(1, 121):
            pages = s.split_columns('あ' * width)
            with self.subTest(width=width):
                self.assertEqual(''.join(pages), 'あ' * width)
                self.assertEqual(len(pages), -(-width // 15))
                self.assertLessEqual(max(map(s.page_width, pages)), 15)
                self.assertLessEqual(max(map(s.page_width, pages))
                                     - min(map(s.page_width, pages)), 1)

    def test_a_half_width_page_holds_thirty_characters(self):
        self.assertEqual(s.page_width('abc'), 1.5)
        self.assertEqual(s.page_width('あa'), 1.5)
        self.assertEqual(s.split_columns('a' * 30), ['a' * 30])
        self.assertEqual(s.split_columns('a' * 40), ['a' * 20, 'a' * 20])
        self.assertEqual(s.split_columns('あ' * 10 + 'ab' * 20),
                         ['あ' * 10 + 'ab' * 5, 'ab' * 15])

    def test_punctuation_never_opens_a_page(self):
        # An even split would open the second page with the bracket, so it moves back one.
        text = 'あ' * 12 + '」' + 'い' * 11
        pages = [page for _, page in s.subtitle_pages(text)]
        self.assertEqual(pages, ['あ' * 11, 'あ」' + 'い' * 11])
        # The space before the bracket is dropped when the page is trimmed, so it counts too.
        self.assertEqual([page for _, page in s.subtitle_pages('あ' * 12 + ' 」' + 'い' * 10)],
                         ['あ' * 11, 'あ 」' + 'い' * 10])
        for text in ('あ' * 15 + '」' + 'い' * 20 + '。', 'あ、' * 30, 'あ' * 12 + '」' + 'い' * 11,
                     'あ' * 12 + ' 」' + 'い' * 10):
            pages = [page for _, page in s.subtitle_pages(text)]
            with self.subTest(text=text):
                self.assertEqual(''.join(pages), text)
                for page in pages:
                    self.assertNotIn(page[0], s.NO_PAGE_START)
                    self.assertLessEqual(s.page_width(page), 15)

    def test_clauses_start_their_own_page_with_the_comma_kept(self):
        pages = s.subtitle_pages('春です、夏です。')
        self.assertEqual(pages, [(0, '春です、'), (4, '夏です。')])

    def test_the_body_stops_at_forty_eight_lines_and_four_kilobytes(self):
        body = s.subtitle_body([(0, 60000., 'あ' * 15 * 60)])
        self.assertEqual(len(body.splitlines()), 48)
        self.assertLessEqual(len(body), 4096)
        self.assertTrue(body.startswith('0\t'.encode('utf-8') + 'あ'.encode('utf-8') * 15))
        short = s.subtitle_body([(0, 60000., 'あ' * 15 * 60)], max_bytes=200)
        self.assertLessEqual(len(short), 200)
        self.assertLess(len(short.splitlines()), 48)
        self.assertTrue(body.startswith(short))

    def test_every_page_of_a_long_reply_is_within_the_contract(self):
        p = self.pipeline(bytes_per_char=2000)
        text = ''.join('これは' + str(i) + '番目の、すこし長めの文です。' for i in range(12))
        reply, audio, body = p.fit(text, Path('/unused'))
        rows = self.lines(body)
        starts = [int(row[0]) for row in rows]
        self.assertEqual(starts[0], 0)
        self.assertEqual(starts, sorted(starts))
        self.assertLess(starts[-1], len(audio) / (s.RATE * 2 // 1000))
        self.assertLessEqual(len(rows), 48)
        self.assertLessEqual(len(body), 4096)
        for start, page in rows:
            self.assertLessEqual(s.page_width(page), 15)
            self.assertNotIn(page[0], s.NO_PAGE_START)
        for (_, page), (_, following) in zip(rows, rows[1:] + [('', 'x' * 8)]):
            # A page is only short when its clause is short, never as the stub of a long one.
            self.assertTrue(s.page_width(page) > 3 or page.endswith(tuple(s.NO_PAGE_START))
                            or s.page_width(page + following) <= 15, page)
        self.assertEqual(''.join(page for _, page in rows), reply[:len(''.join(
            page for _, page in rows))])


if __name__ == '__main__':
    unittest.main()
