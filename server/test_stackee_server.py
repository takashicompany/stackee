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

import stackee_server as s


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
        audio = self.pipeline.synthesize('こんにちは & 元気？', Path('/unused'))
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
            return {'state': 'done', 'transcript': 'test', 'reply': 'reply'}, b'\0\1' * 100
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


if __name__ == '__main__':
    unittest.main()
