"""CSTM keys (POST /key), the inbox (GET /inbox), POST /say and stackee-say.

Fake synthesis and fake agents only: nothing here speaks, and no real Codex / Claude runs.
"""
import http.client
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import Mock, patch

import stackee_agent
import stackee_server as s
from test_stackee_agent import FAKE_CLAUDE, JPEG
from test_stackee_server import wav

HERE = Path(__file__).resolve().parent
PCM = b'\0\1' * 1600   # what the fake engine answers for any sentence: 0.1 s


class KeySettingsTests(unittest.TestCase):
    def test_the_shipped_defaults_leave_every_key_unset(self):
        raw = json.loads((HERE / 'agent/defaults/keys.json').read_text(encoding='utf-8'))
        self.assertEqual(s.normalize_keys(raw), s.default_keys())
        self.assertEqual(list(raw), ['CSTM_%d' % n for n in range(10)])

    def test_missing_runtime_file_is_restored_from_defaults_and_edits_are_kept(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'defaults').mkdir()
            shutil.copyfile(HERE / 'agent/defaults/keys.json', root / 'defaults/keys.json')
            keys = s.KeySettings(root)
            self.assertTrue((root / 'keys.json').is_file())
            keys.write({'CSTM_1': {'mode': 'command', 'command': 'true'}})
            s.KeySettings(root)
            self.assertEqual(keys.read()['CSTM_1']['mode'], 'command')
            self.assertEqual(keys.read()['CSTM_1']['timeout'], 300)
            self.assertEqual(keys.read()['CSTM_0']['mode'], 'none')

    def test_validation(self):
        good = {'CSTM_0': {'mode': 'prompt', 'prompt': 'やあ', 'command': 'x', 'timeout': 600}}
        self.assertEqual(s.normalize_keys(good)['CSTM_0'],
                         {'mode': 'prompt', 'prompt': 'やあ', 'command': 'x', 'timeout': 600})
        for bad in ([], {'CSTM_10': {}}, {'KC_A': {}}, {'CSTM_0': 'prompt'},
                    {'CSTM_0': {'mode': 'shell'}},
                    {'CSTM_0': {'mode': 'prompt', 'prompt': '  '}},
                    {'CSTM_0': {'mode': 'command', 'command': ''}},
                    {'CSTM_0': {'mode': 'command', 'command': 'a\0b'}},
                    {'CSTM_0': {'prompt': 3}},
                    {'CSTM_0': {'timeout': 0}}, {'CSTM_0': {'timeout': 601}},
                    {'CSTM_0': {'timeout': 1.5}}, {'CSTM_0': {'timeout': True}},
                    {'CSTM_0': {'timeout': '300'}}):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                s.normalize_keys(bad)

    def test_say_url_is_built_from_the_listening_address(self):
        self.assertEqual(s.say_url(('0.0.0.0', 8766)), 'http://127.0.0.1:8766/say')
        self.assertEqual(s.say_url(('192.168.1.10', 8766)), 'http://192.168.1.10:8766/say')
        self.assertEqual(s.say_url(('::', 8766, 0, 0)), 'http://[::1]:8766/say')

    def test_only_this_machine_counts_as_local(self):
        for peer, own, expected in (('127.0.0.1', '127.0.0.1', True),
                                    ('127.0.0.1', '192.168.1.10', True),
                                    ('::1', '::1', True),
                                    ('::ffff:127.0.0.1', '::ffff:192.168.1.10', True),
                                    ('192.168.1.10', '192.168.1.10', True),
                                    ('192.168.1.20', '192.168.1.10', False),
                                    ('100.64.0.5', '192.168.1.10', False),
                                    ('garbage', '192.168.1.10', False)):
            with self.subTest(peer=peer, own=own):
                self.assertIs(s.local_address(peer, own), expected)


class SayPipelineTests(unittest.TestCase):
    def pipeline(self):
        p = s.Pipeline('unused', echo=True)
        p.calls = []
        def synthesize(text, root):
            p.calls.append(text)
            return PCM
        p.synthesize = synthesize
        return p

    def test_voice_goes_through_the_reply_path(self):
        p = self.pipeline()
        reply, audio, subtitles = p.say('ビルドが終わりました。詳しくは[ログ](https://x.example)で。https://ci.example.com/1')
        self.assertEqual(reply, 'ビルドが終わりました。詳しくはログで。')
        self.assertEqual(p.calls, ['ビルドが終わりました。', '詳しくはログで。'])
        self.assertEqual(len(audio), 2 * len(PCM) + len(s.SILENCE))
        self.assertTrue(subtitles.startswith('0\tビルドが終わりました。\n'.encode()))

    def test_without_voice_only_captions_at_a_steady_pace(self):
        p = self.pipeline()
        reply, audio, subtitles = p.say('ひとつ目です。ふたつ目、みっつ目です。', voice=False)
        self.assertEqual((reply, audio, p.calls), ('ひとつ目です。ふたつ目、みっつ目です。', b'', []))
        self.assertEqual(subtitles.decode(), '0\tひとつ目です。\n2500\tふたつ目、\n5000\tみっつ目です。\n')
        long = 'あ' * 40 + '。'
        body = s.subtitle_only_body(long)
        self.assertEqual([line.split('\t')[1] for line in body.decode().splitlines()],
                         [page for _, page in s.subtitle_pages(long)])

    def test_nothing_speakable_is_refused(self):
        p = self.pipeline()
        with self.assertRaisesRegex(ValueError, 'nothing_to_say'):
            p.say('https://example.com')
        self.assertEqual(p.calls, [])

    def test_syntheses_take_turns(self):
        p = s.Pipeline('unused', echo=True)
        active, peak = [0], [0]
        lock = threading.Lock()
        def synthesize(text, root):
            with lock:
                active[0] += 1
                peak[0] = max(peak[0], active[0])
            time.sleep(.05)
            with lock:
                active[0] -= 1
            return PCM
        p.synthesize = synthesize
        threads = [threading.Thread(target=p.say, args=('ひとつ。ふたつ。',)) for _ in range(3)]
        threads.append(threading.Thread(target=p.fit, args=('みっつ。', Path('/unused'))))
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(5)
        self.assertEqual(peak[0], 1)


class KeyHttpTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name).resolve()
        (self.root / 'defaults').mkdir()
        shutil.copyfile(HERE / 'agent/defaults/keys.json', self.root / 'defaults/keys.json')
        self.pipeline = s.Pipeline('unused', echo=True)
        self.spoken = []
        def synthesize(text, root):
            self.spoken.append(text)
            return PCM
        self.pipeline.synthesize = synthesize
        self.keys = s.KeySettings(self.root)
        self.server = s.ThreadingHTTPServer(('127.0.0.1', 0), s.Handler)
        self.server.jobs = s.Jobs(self.pipeline, keys=self.keys,
                                  say_url=s.say_url(self.server.server_address))
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.release = self.root / 'release'

    def tearDown(self):
        self.release.touch()
        self.server.jobs.close()
        if self.server.jobs.worker:
            self.server.jobs.worker.join(5)
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

    def press(self, key, **extra):
        headers = {'Content-Type': 'application/json'}
        headers.update(extra)
        status, headers, body = self.request('POST', '/key', {'key': key}, headers)
        return status, headers, json.loads(body)

    def say(self, text, voice=True):
        status, _, body = self.request('POST', '/say', {'text': text, 'voice': voice},
                                       {'Content-Type': 'application/json'}, timeout=10)
        return status, json.loads(body)

    def set_key(self, key, **entry):
        keys = self.keys.read()
        keys[key] = dict(keys[key], **entry)
        self.keys.write(keys)

    def job(self, created):
        self.server.jobs.worker.join(10)
        return json.loads(self.request('GET', created['status_url'])[2])

    def hold(self, key='CSTM_2'):
        """A command that keeps the slot until the test touches the release file."""
        self.set_key(key, mode='command',
                     command='while [ ! -e "%s" ]; do sleep 0.02; done' % self.release)
        status, _, created = self.press(key)
        self.assertEqual(status, 202)
        return created

    # POST /key --------------------------------------------------------------------

    def test_an_unset_key_is_only_logged(self):
        with self.assertLogs(level='INFO') as logs:
            status, _, body = self.press('CSTM_3')
        self.assertEqual((status, body), (200, {'state': 'ignored', 'key': 'CSTM_3'}))
        self.assertTrue(any('key-press' in line and 'CSTM_3' in line for line in logs.output))
        self.assertIsNone(self.server.jobs.worker)
        self.assertEqual(self.server.jobs.recent_presses()[0]['result'], 'ignored')

    def test_bad_key_requests_do_no_work(self):
        json_type = {'Content-Type': 'application/json'}
        for body, headers, expected in (
                ({'key': 'CSTM_10'}, json_type, 400), ({'key': 'KC_A'}, json_type, 400),
                ({'key': 3}, json_type, 400), ([], json_type, 400), (b'{', json_type, 400),
                ({'key': 'CSTM_1'}, {'Content-Type': 'text/plain'}, 415),
                ({'key': 'CSTM_1'}, dict(json_type, Origin='http://evil.example'), 403),
                ({'key': 'CSTM_1', 'pad': 'x' * 300}, json_type, 413),
                (b'', dict(json_type, **{'Content-Length': '0'}), 413)):
            with self.subTest(body=body, headers=headers):
                self.assertEqual(self.request('POST', '/key', body, headers)[0], expected)
        self.assertEqual(self.request('GET', '/key')[0], 404)
        self.assertIsNone(self.server.jobs.worker)

    def test_a_prompt_key_is_a_look_without_the_photo(self):
        self.set_key('CSTM_0', mode='prompt', prompt='今日の予定を教えて')
        self.pipeline.echo = False
        self.pipeline.agent = Mock()
        self.pipeline.agent.ask.return_value = '予定は2件です。[詳細](https://example.com)を見てね。'
        status, headers, created = self.press('CSTM_0')
        self.assertEqual(status, 202)
        self.assertEqual(set(created), {'id', 'status_url', 'mode'})
        self.assertEqual(created['mode'], 'prompt')
        self.assertEqual(headers['Location'], created['status_url'])
        state = self.job(created)
        self.assertEqual(self.pipeline.agent.ask.call_args.args, ('今日の予定を教えて',))
        self.assertEqual(set(state), {'state', 'transcript', 'reply', 'timings', 'audio_url',
                                      'audio_bytes', 'sample_rate', 'channels', 'sample_width',
                                      'subtitles_url', 'subtitles'})
        self.assertEqual((state['state'], state['transcript'], state['reply']),
                         ('done', '', '予定は2件です。詳細を見てね。'))
        self.assertEqual(self.spoken, ['予定は2件です。', '詳細を見てね。'])
        audio = self.request('GET', state['audio_url'])[2]
        self.assertEqual(len(audio), state['audio_bytes'])
        self.assertEqual(self.request('GET', state['subtitles_url'])[2].decode(), state['subtitles'])

    def test_a_prompt_key_joins_the_saved_conversation(self):
        agent_dir = self.root / 'agent'
        agent_dir.mkdir()
        (agent_dir / 'AGENTS.md').write_text('Codex', encoding='utf-8')
        (agent_dir / 'CLAUDE.md').write_text('Claude', encoding='utf-8')
        (agent_dir / 'agent.json').write_text(json.dumps({'agent': 'claude'}), encoding='utf-8')
        fake = agent_dir / 'fake-claude'
        fake.write_text('#!' + sys.executable + '\n' + FAKE_CLAUDE)
        fake.chmod(0o700)
        pipeline = s.Pipeline('unused', claude=str(fake), agent_dir=agent_dir)
        pipeline.synthesize = lambda text, root: PCM
        try:
            pipeline.look(JPEG)
            conversation = pipeline.agent.status()['conversations']['claude']
            self.assertTrue(conversation)
            result, _ = pipeline.prompt('recall-image')
            self.assertEqual(result['reply'], 'saw %d bytes ffd8' % len(JPEG))
            self.assertEqual(result['transcript'], '')
            self.assertEqual(pipeline.agent.status()['conversations']['claude'], conversation)
        finally:
            pipeline.close()

    def test_a_command_runs_in_the_agent_directory_with_its_environment(self):
        self.set_key('CSTM_3', mode='command', command=(
            'echo "key=$STACKEE_KEY"; echo "url=$STACKEE_SAY_URL"; pwd; '
            'command -v stackee-say; echo oops >&2'))
        with self.assertLogs(level='INFO') as logs:
            status, headers, created = self.press('CSTM_3')
            self.assertEqual((status, created['mode']), (202, 'command'))
            self.assertEqual(headers['Location'], created['status_url'])
            state = self.job(created)
        self.assertEqual(state, {'state': 'done', 'reply': '', 'exit_code': 0})
        self.assertEqual(self.request('GET', created['status_url'] + '/audio')[0], 404)
        self.assertEqual(self.request('GET', created['status_url'] + '/subtitles')[0], 404)
        line = next(line for line in logs.output if 'key-command ' in line)
        logged = json.loads(line.split('key-command ', 1)[1])
        out = logged['stdout'].splitlines()
        self.assertEqual(out[0], 'key=CSTM_3')
        self.assertEqual(out[1], 'url=http://127.0.0.1:%d/say' % self.server.server_address[1])
        self.assertEqual(Path(out[2]).resolve(), self.root)
        self.assertEqual(Path(out[3]), s.BIN_DIR / 'stackee-say')
        self.assertEqual((logged['stderr'], logged['exit_code']), ('oops\n', 0))
        self.assertEqual(self.server.jobs.recent_presses()[0]['result'], 'done')

    def test_a_failing_command_is_a_short_error(self):
        self.set_key('CSTM_3', mode='command', command='echo failing >&2; exit 2')
        state = self.job(self.press('CSTM_3')[2])
        self.assertEqual(state, {'state': 'error', 'error': 'CSTM_3 失敗 (終了コード 2)'})
        self.assertEqual(self.server.jobs.recent_presses()[0]['error'], 'CSTM_3 失敗 (終了コード 2)')

    def test_a_command_past_its_time_is_killed(self):
        self.set_key('CSTM_4', mode='command', command='sleep 30; echo never', timeout=1)
        started = time.monotonic()
        state = self.job(self.press('CSTM_4')[2])
        self.assertEqual(state, {'state': 'error', 'error': 'CSTM_4 時間切れ'})
        self.assertLess(time.monotonic() - started, 5)
        self.assertFalse(self.server.jobs.busy)

    def test_huge_output_only_leaves_its_tail_in_the_log(self):
        self.set_key('CSTM_5', mode='command',
                     command='i=0; while [ $i -lt 2000 ]; do echo "line $i"; i=$((i+1)); done')
        with self.assertLogs(level='INFO') as logs:
            self.job(self.press('CSTM_5')[2])
        line = next(line for line in logs.output if 'key-command ' in line)
        stdout = json.loads(line.split('key-command ', 1)[1])['stdout']
        self.assertTrue(stdout.startswith('…'))
        self.assertTrue(stdout.endswith('line 1999\n'))
        self.assertLessEqual(len(stdout.encode()), s.KEY_OUTPUT_TAIL + 3)

    def test_a_background_child_does_not_hold_the_slot(self):
        self.set_key('CSTM_6', mode='command', command='sleep 5 & echo started')
        started = time.monotonic()
        state = self.job(self.press('CSTM_6')[2])
        self.assertEqual(state['state'], 'done')
        self.assertLess(time.monotonic() - started, 3)

    def test_a_command_holds_the_slot_shared_with_talk_and_look(self):
        created = self.hold()
        json_type = {'Content-Type': 'application/json'}
        self.set_key('CSTM_0', mode='prompt', prompt='x')
        self.assertEqual(self.request('POST', '/key', {'key': 'CSTM_0'}, json_type)[0], 409)
        self.assertEqual(self.request('POST', '/key', {'key': 'CSTM_2'}, json_type)[0], 409)
        self.assertEqual(self.request('POST', '/talk', wav(), {'Content-Type': 'audio/wav'})[0], 409)
        self.assertEqual(self.request('POST', '/look', JPEG, {'Content-Type': 'image/jpeg'})[0], 409)
        # An unset key and the inbox never need the slot.
        self.assertEqual(self.press('CSTM_9')[0], 200)
        self.assertEqual(json.loads(self.request('GET', '/inbox')[2]), {'state': 'empty', 'seq': 0})
        self.assertEqual(self.say('処理中でも話せます。')[0], 200)
        self.assertEqual(json.loads(self.request('GET', created['status_url'])[2]),
                         {'state': 'processing'})
        self.assertEqual([p['result'] for p in self.server.jobs.recent_presses()][:2],
                         ['ignored', 'busy'])
        self.release.touch()
        self.assertEqual(self.job(created)['state'], 'done')

    def test_talk_in_progress_answers_a_key_with_409(self):
        released = threading.Event()
        self.pipeline.run = lambda *args, **kwargs: (released.wait(5), 'test')[1]
        self.assertEqual(self.request('POST', '/talk', wav(), {'Content-Type': 'audio/wav'})[0], 202)
        self.set_key('CSTM_1', mode='command', command='true')
        self.assertEqual(self.press('CSTM_1')[0], 409)
        released.set()
        self.server.jobs.worker.join(5)
        self.assertEqual(self.press('CSTM_1')[0], 202)
        self.server.jobs.worker.join(5)

    def test_a_saved_setting_applies_to_the_next_press(self):
        self.assertEqual(self.press('CSTM_7')[0], 200)
        self.set_key('CSTM_7', mode='command', command='true')
        self.assertEqual(self.press('CSTM_7')[0], 202)
        self.server.jobs.worker.join(5)

    # stackee-say → inbox → device ------------------------------------------------

    def test_a_command_speaks_through_stackee_say_and_one_long_poll_sees_both(self):
        self.set_key('CSTM_8', mode='command',
                     command='stackee-say "ビルドが終わりました。" && stackee-say --no-voice "字幕だけ"')
        before = json.loads(self.request('GET', '/inbox')[2])['seq']
        created = self.press('CSTM_8')[2]
        status, _, body = self.request(
            'GET', '/inbox?after=%d&wait=10&job=%s' % (before, created['id']), timeout=15)
        first = json.loads(body)
        self.assertEqual(status, 200)
        self.assertEqual(first['state'], 'say')
        self.assertEqual(first['seq'], before + 1)
        self.assertEqual(first['reply'], 'ビルドが終わりました。')
        self.assertEqual(first['audio_url'], '/inbox/%d/audio' % first['seq'])
        self.assertEqual((first['sample_rate'], first['channels'], first['sample_width']),
                         (16000, 1, 2))
        self.assertEqual(first['subtitles'], '0\tビルドが終わりました。\n')
        status, headers, audio = self.request('GET', first['audio_url'])
        self.assertEqual((status, headers['Content-Type']), (200, 'application/octet-stream'))
        self.assertEqual(len(audio), first['audio_bytes'])
        self.assertLessEqual(s.peak(audio), 8191)
        self.server.jobs.worker.join(10)
        second = json.loads(self.request(
            'GET', '/inbox?after=%d&wait=10&job=%s' % (first['seq'], created['id']))[2])
        self.assertEqual((second['state'], second['reply'], second['job_state']),
                         ('say', '字幕だけ', 'done'))
        self.assertNotIn('audio_url', second)
        self.assertNotIn('audio_bytes', second)
        self.assertEqual(second['subtitles'], '0\t字幕だけ\n')
        self.assertEqual(self.request('GET', '/inbox/%d/audio' % second['seq'])[0], 404)
        last = json.loads(self.request(
            'GET', '/inbox?after=%d&wait=10&job=%s' % (second['seq'], created['id']))[2])
        self.assertEqual(last, {'state': 'empty', 'seq': second['seq'], 'job_state': 'done'})
        self.assertEqual(self.spoken, ['ビルドが終わりました。'])

    def test_stackee_say_reads_stdin_and_reports_failures(self):
        import subprocess
        script = str(s.BIN_DIR / 'stackee-say')
        env = dict(os.environ, STACKEE_SAY_URL=self.server.jobs.say_url)
        done = subprocess.run([script], input='標準入力から。'.encode(), env=env,
                              capture_output=True, timeout=10)
        self.assertEqual((done.returncode, done.stdout), (0, b''))
        item = json.loads(self.request('GET', '/inbox?after=0')[2])
        self.assertEqual(item['reply'], '標準入力から。')
        self.assertIn('audio_url', item)
        for argv, stdin, expected in (([script, 'https://example.com'], b'', 1),
                                      ([script], b'  ', 2)):
            failed = subprocess.run(argv, input=stdin, env=env, capture_output=True, timeout=10)
            self.assertEqual(failed.returncode, expected, failed.stderr)
            self.assertTrue(failed.stderr)
        unreachable = dict(env, STACKEE_SAY_URL='http://127.0.0.1:9/say')
        failed = subprocess.run([script, 'x'], env=unreachable, capture_output=True, timeout=10)
        self.assertEqual(failed.returncode, 1)

    def test_the_inbox_without_a_query_only_tells_the_latest_seq(self):
        self.assertEqual(json.loads(self.request('GET', '/inbox')[2]), {'state': 'empty', 'seq': 0})
        self.say('ひとつ。')
        self.say('ふたつ。')
        self.assertEqual(json.loads(self.request('GET', '/inbox')[2]), {'state': 'empty', 'seq': 2})

    def test_after_returns_the_oldest_newer_utterance_at_once(self):
        for text in ('いち。', 'に。', 'さん。'):
            self.say(text)
        item = json.loads(self.request('GET', '/inbox?after=1&wait=25')[2])
        self.assertEqual((item['seq'], item['reply']), (2, 'に。'))
        self.assertNotIn('job_state', item)
        self.assertEqual(json.loads(self.request('GET', '/inbox?after=3')[2]),
                         {'state': 'empty', 'seq': 3})

    def test_a_long_poll_answers_as_soon_as_something_is_said(self):
        timer = threading.Timer(.3, lambda: self.server.jobs.inbox.put('あとから', PCM, b''))
        timer.start()
        started = time.monotonic()
        item = json.loads(self.request('GET', '/inbox?after=0&wait=10', timeout=15)[2])
        self.assertEqual((item['state'], item['seq'], item['reply']), ('say', 1, 'あとから'))
        self.assertLess(time.monotonic() - started, 3)
        timer.join()

    def test_a_long_poll_that_hears_nothing_says_empty(self):
        started = time.monotonic()
        self.assertEqual(json.loads(self.request('GET', '/inbox?after=0&wait=1')[2]),
                         {'state': 'empty', 'seq': 0})
        self.assertGreaterEqual(time.monotonic() - started, 1)

    def test_a_long_poll_following_a_job_returns_when_it_ends(self):
        created = self.hold()
        timer = threading.Timer(.3, self.release.touch)
        timer.start()
        started = time.monotonic()
        body = json.loads(self.request(
            'GET', '/inbox?after=0&wait=10&job=' + created['id'], timeout=15)[2])
        self.assertEqual(body, {'state': 'empty', 'seq': 0, 'job_state': 'done'})
        self.assertLess(time.monotonic() - started, 3)
        timer.join()
        self.set_key('CSTM_2', mode='command', command='exit 3')
        created = self.press('CSTM_2')[2]
        body = json.loads(self.request(
            'GET', '/inbox?after=0&wait=10&job=' + created['id'], timeout=15)[2])
        self.assertEqual(body, {'state': 'empty', 'seq': 0, 'job_state': 'error',
                                'error': 'CSTM_2 失敗 (終了コード 3)'})
        self.assertEqual(json.loads(self.request('GET', '/inbox?after=0&wait=10&job=' + 'f' * 32)[2]),
                         {'state': 'empty', 'seq': 0, 'job_state': 'not_found'})

    def test_bad_inbox_queries(self):
        for query in ('?wait=5', '?after=x', '?after=-1', '?after=0&wait=x', '?after=0&job=zz',
                      '?after=0&x=1', '?after=0&after=1', '?after', '?after=0&wait=5000'):
            with self.subTest(query=query):
                self.assertEqual(self.request('GET', '/inbox' + query)[0], 400)
        for path in ('/inbox/', '/inbox/x/audio', '/inbox/1', '/inbox/1/audio?x=1'):
            self.assertEqual(self.request('GET', path)[0], 404, path)

    def test_the_wait_is_capped(self):
        with patch.object(s, 'MAX_INBOX_WAIT', 1):
            started = time.monotonic()
            self.assertEqual(self.request('GET', '/inbox?after=0&wait=25')[0], 200)
            self.assertLess(time.monotonic() - started, 3)

    def test_the_inbox_keeps_sixteen_for_five_minutes(self):
        inbox = self.server.jobs.inbox
        for n in range(17):
            inbox.put('%d' % n, PCM, b'')
        self.assertEqual(inbox.summary(), {'count': 16, 'seq': 17})
        self.assertEqual(self.request('GET', '/inbox/1/audio')[0], 404)
        self.assertEqual(self.request('GET', '/inbox/2/audio')[2], PCM)
        self.assertEqual(json.loads(self.request('GET', '/inbox?after=0')[2])['seq'], 2)
        for item in inbox.items:
            item['created'] -= 301
        self.assertEqual(self.request('GET', '/inbox/17/audio')[0], 404)
        self.assertEqual(json.loads(self.request('GET', '/inbox?after=0')[2]),
                         {'state': 'empty', 'seq': 17})
        self.assertEqual(inbox.summary(), {'count': 0, 'seq': 17})

    def test_an_utterance_fits_the_device_buffer(self):
        subtitles = ('999999\t' + 'あ' * 26 + '\n').encode() * 48
        seq = self.server.jobs.inbox.put('あ' * 1300, PCM, subtitles)
        raw = self.request('GET', '/inbox?after=%d' % (seq - 1))[2]
        self.assertLessEqual(len(raw), s.MAX_JOB_BYTES)
        self.assertTrue(subtitles.decode().startswith(json.loads(raw)['subtitles']))

    # POST /say --------------------------------------------------------------------

    def test_say_needs_a_local_client(self):
        with patch.object(s.Handler, 'local_client', return_value=False):
            status, body = self.say('だれ？')
        self.assertEqual((status, body), (403, {'error': 'local_only'}))
        self.assertEqual(self.server.jobs.inbox.summary()['seq'], 0)

    def test_say_rejects_bad_requests(self):
        json_type = {'Content-Type': 'application/json'}
        for body, headers, expected in (
                ({'text': 'x'}, {'Content-Type': 'text/plain'}, 415),
                ({'text': 'x'}, dict(json_type, Origin='http://127.0.0.1'), 403),
                ({'text': ''}, json_type, 400), ({'text': '  '}, json_type, 400),
                ({'voice': True}, json_type, 400), ({'text': 'x', 'voice': 'no'}, json_type, 400),
                ([], json_type, 400), (b'{', json_type, 400),
                ({'text': 'あ' * 1366}, json_type, 413),
                ({'text': 'https://example.com'}, json_type, 400)):
            with self.subTest(body=body, headers=headers):
                self.assertEqual(self.request('POST', '/say', body, headers)[0], expected)
        self.assertEqual(self.say('あ' * 1365, voice=False)[0], 200)
        self.assertEqual(self.request('GET', '/say')[0], 404)

    def test_a_failed_synthesis_is_500_and_queues_nothing(self):
        def refuse(text, root):
            raise RuntimeError('VOICEVOX is down')
        self.pipeline.synthesize = refuse
        status, body = self.say('こんにちは。')
        self.assertEqual((status, body), (500, {'error': 'VOICEVOX is down'}))
        self.assertEqual(self.server.jobs.inbox.summary(), {'count': 0, 'seq': 0})

    def test_existing_job_responses_are_untouched(self):
        self.assertEqual(set(json.loads(self.request('GET', '/health')[2])),
                         {'ok', 'agent', 'busy', 'conversation'})
        self.assertEqual(self.request('GET', '/jobs/' + '0' * 32 + '?wait=5')[0], 404)


class KeyAdminTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name).resolve()
        (self.root / 'defaults').mkdir()
        for name, text in (('AGENTS.md', 'Codex'), ('CLAUDE.md', 'Claude'),
                           ('agent.json', json.dumps({'agent': 'claude'}))):
            (self.root / 'defaults' / name).write_text(text, encoding='utf-8')
        shutil.copyfile(HERE / 'agent/defaults/keys.json', self.root / 'defaults/keys.json')
        claude = self.root / 'fake-claude'
        claude.write_text('#!' + sys.executable + '\n' + FAKE_CLAUDE)
        claude.chmod(0o700)
        self.agent = stackee_agent.Agent(self.root, '/nonexistent/codex', str(claude))
        self.agent.start()

        class FakePipeline:
            agent = self.agent

        self.server = s.ThreadingHTTPServer(('127.0.0.1', 0), s.Handler)
        self.server.jobs = s.Jobs(FakePipeline(), keys=s.KeySettings(self.root))
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
        result = resp.status, resp.read()
        conn.close()
        return result

    def put(self, keys, headers=None):
        extra = {'X-Stackee-Admin': '1', 'Content-Type': 'application/json'}
        extra.update(headers or {})
        return self.request('PUT', '/admin/api/keys', json.dumps(keys, ensure_ascii=False).encode(), extra)

    def test_state_carries_keys_presses_and_the_inbox(self):
        self.request('POST', '/key', json.dumps({'key': 'CSTM_4'}),
                     {'Content-Type': 'application/json'})
        self.server.jobs.inbox.put('やあ', b'', '0\tやあ\n'.encode())
        data = json.loads(self.request('GET', '/admin/api/state')[1])
        self.assertEqual(data['keys'], s.default_keys())
        self.assertIsNone(data['keys_error'])
        self.assertEqual(data['choices']['key_modes'], ['none', 'prompt', 'command'])
        self.assertEqual(data['choices']['key_timeout_max'], 600)
        press = data['key_presses'][0]
        self.assertEqual((press['key'], press['mode'], press['result']), ('CSTM_4', 'none', 'ignored'))
        self.assertTrue(press['at'])
        self.assertEqual(data['inbox'], {'count': 1, 'seq': 1})
        # The existing fields are all still there.
        for name in ('config', 'instructions', 'status', 'busy'):
            self.assertIn(name, data)

    def test_keys_are_saved_at_once(self):
        keys = s.default_keys()
        keys['CSTM_1'] = {'mode': 'prompt', 'prompt': '天気は？', 'command': '', 'timeout': 300}
        keys['CSTM_2'] = {'mode': 'command', 'command': 'make -C ~/x', 'prompt': '', 'timeout': 45}
        status, body = self.put(keys)
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)['keys'], keys)
        saved = json.loads((self.root / 'keys.json').read_text(encoding='utf-8'))
        self.assertEqual(saved, keys)
        self.assertEqual(json.loads(self.request('GET', '/admin/api/state')[1])['keys'], keys)

    def test_key_writes_are_guarded_and_validated(self):
        original = (self.root / 'keys.json').read_bytes()
        good = {'CSTM_1': {'mode': 'command', 'command': 'true'}}
        self.assertEqual(self.request('PUT', '/admin/api/keys', json.dumps(good),
                                      {'Content-Type': 'application/json'})[0], 403)
        self.assertEqual(self.put(good, {'Origin': 'http://evil.example'})[0], 403)
        self.assertEqual(self.put({'CSTM_1': {'mode': 'prompt', 'prompt': ''}})[0], 400)
        self.assertEqual(self.put({'CSTM_1': {'mode': 'command', 'command': 'x' * 70000}})[0], 413)
        self.assertEqual((self.root / 'keys.json').read_bytes(), original)

    def test_a_broken_keys_file_is_reported_not_fatal(self):
        (self.root / 'keys.json').write_text('{', encoding='utf-8')
        status, body = self.request('GET', '/admin/api/state')
        data = json.loads(body)
        self.assertEqual(status, 200)
        self.assertIsNone(data['keys'])
        self.assertTrue(data['keys_error'])
        status, body = self.request('POST', '/key', json.dumps({'key': 'CSTM_0'}),
                                    {'Content-Type': 'application/json'})
        self.assertEqual((status, json.loads(body)), (500, {'error': 'keys_unreadable'}))


if __name__ == '__main__':
    unittest.main()
