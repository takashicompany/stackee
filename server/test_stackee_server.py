import array
import http.client
import io
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
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


class AudioTests(unittest.TestCase):
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

    def test_pipeline_uses_codex_final_file_and_history(self):
        p = s.Pipeline('unused')
        calls = []
        def run(argv, cwd, input=None):
            calls.append((argv, input))
            if argv[0] == 'whisper-cli':
                return 'こんにちは'
            if argv[0] == 'codex':
                self.assertIn('exec', argv)
                self.assertIn('read-only', argv)
                self.assertIn('--ignore-user-config', argv)
                self.assertIn('ユーザー: こんにちは', input.decode())
                Path(argv[argv.index('-o') + 1]).write_text('こんにちは。')
                return 'progress log, not the answer'
            Path(argv[argv.index('-o') + 1]).write_bytes(wav())
            return ''
        p.run = run
        result, audio = p(wav())
        self.assertEqual(result['reply'], 'こんにちは。')
        self.assertEqual(len(audio), 16000)
        self.assertEqual(p.history, [{'user': 'こんにちは', 'assistant': 'こんにちは。'}])
        self.assertEqual([x[0][0] for x in calls], ['whisper-cli', 'codex', 'say'])

    def test_codex_empty_final_is_error(self):
        p = s.Pipeline('unused')
        p.run = lambda argv, cwd, input=None: 'こんにちは'
        with self.assertRaisesRegex(RuntimeError, 'no final message'):
            p(wav())
        self.assertEqual(p.history, [])

    def test_timeout_kills_subprocess(self):
        import sys
        p = s.Pipeline('unused', timeout=.05)
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(RuntimeError, 'timed out'):
                p.run([sys.executable, '-c', 'import time; time.sleep(10)'], tmp)
        self.assertIsNone(p.process)


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
