import json
import os
from pathlib import Path
import signal
import sys
import tempfile
import threading
import time
import unittest

from stackee_agent import Agent, CodexAgent, normalize_config


FAKE_CODEX = r'''
import json, os, sys, time, uuid
from pathlib import Path
assert sys.argv[1:] == ['app-server', '--listen', 'stdio://']
store = Path('.state/fake-history.json')
instructions = ''
def send(value):
    print(json.dumps(value), flush=True)
for line in sys.stdin:
    msg = json.loads(line)
    method, params = msg.get('method'), msg.get('params', {})
    if method == 'initialize':
        send({'id': msg['id'], 'result': {}})
    elif method in ('thread/start', 'thread/resume'):
        assert params['cwd'] == os.getcwd()
        assert params['sandbox'] == 'read-only'
        assert params['approvalPolicy'] == 'never'
        instructions = params['developerInstructions']
        if method == 'thread/start':
            state = {'id': str(uuid.uuid4()), 'messages': []}
        else:
            if not store.exists():
                send({'id': msg['id'], 'error': {'message': 'session not found'}})
                continue
            state = json.loads(store.read_text())
            assert state['id'] == params['threadId']
        send({'id': msg['id'], 'result': {'thread': {'id': state['id']}, 'model': params['model']}})
    elif method == 'turn/start':
        assert params['threadId'] == state['id']
        text = params['input'][0]['text']
        state['messages'].append(text)
        store.write_text(json.dumps(state))
        turn = str(uuid.uuid4())
        if text == 'timeout':
            send({'id': msg['id'], 'result': {'turn': {'id': turn}}})
            time.sleep(20)
            continue
        if text == 'recall':
            reply = next(x for x in state['messages'] if x.startswith('remember '))
        elif text == 'instructions':
            reply = instructions
        else:
            reply = 'reply ' + text
        # Events may arrive before the response to turn/start.
        send({'method': 'turn/completed', 'params': {'threadId': 'unrelated', 'turn': {'id': turn, 'status': 'completed'}}})
        for phase, value in [('commentary', 'do not speak'), ('final_answer', reply)]:
            if text == 'missing-final' and phase == 'final_answer':
                continue
            send({'method': 'item/completed', 'params': {'threadId': state['id'], 'turnId': turn,
                 'item': {'type': 'agentMessage', 'phase': phase, 'text': value}}})
        send({'method': 'turn/completed', 'params': {'threadId': state['id'],
             'turn': {'id': turn, 'status': 'failed' if text == 'fail' else 'completed', 'error': {'message': 'test failure'}}}})
        send({'id': msg['id'], 'result': {'turn': {'id': turn}}})
'''


FAKE_CLAUDE = r'''
import json, sys, time, uuid
from pathlib import Path
args = sys.argv[1:]
def value(name):
    return args[args.index(name) + 1] if name in args else None
assert '-p' in args
assert value('--output-format') == 'json'
assert value('--model')
assert value('--effort')
assert value('--tools') == 'WebSearch,WebFetch'
assert value('--allowedTools') == 'WebSearch,WebFetch'
assert '--strict-mcp-config' in args
assert value('--setting-sources') == 'project'
assert value('--permission-mode') == 'plan'
resume = value('--resume')
text = sys.stdin.read()
store = Path('.state/fake-claude.json')
store.parent.mkdir(parents=True, exist_ok=True)
book = json.loads(store.read_text()) if store.exists() else {}

def emit(payload):
    print(json.dumps(payload, ensure_ascii=False), flush=True)

if resume is not None:
    if resume not in book:
        emit({'type': 'result', 'subtype': 'error_during_execution', 'is_error': True,
              'session_id': resume, 'result': 'No conversation found with session ID: ' + resume})
        sys.exit(1)
    ident = resume
else:
    ident = str(uuid.uuid4())
    book[ident] = []
book[ident].append(text)
store.write_text(json.dumps(book, ensure_ascii=False))
if text == 'timeout':
    time.sleep(20)
    sys.exit(0)
if text == 'slow':
    time.sleep(1)
if text == 'recall':
    reply = next(x for x in book[ident] if x.startswith('remember '))
elif text == 'instructions':
    reply = Path('CLAUDE.md').read_text(encoding='utf-8')
elif text == 'fail':
    emit({'type': 'result', 'subtype': 'error_during_execution', 'is_error': True,
          'session_id': ident, 'result': 'test failure'})
    sys.exit(0)
elif text == 'missing-final':
    reply = ''
else:
    reply = 'reply ' + text
emit({'type': 'result', 'subtype': 'success', 'is_error': False, 'session_id': ident,
      'model': value('--model'), 'result': reply})
'''


class AgentTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / 'AGENTS.md').write_text('Original instructions')
        (self.root / 'agent.json').write_text(json.dumps({'model': 'test-model', 'effort': 'none'}))
        self.executable = self.root / 'fake-codex'
        self.executable.write_text('#!' + sys.executable + '\n' + FAKE_CODEX)
        self.executable.chmod(0o700)
        self.agents = []

    def agent(self, **kwargs):
        agent = CodexAgent(self.root, str(self.executable), **kwargs)
        self.agents.append(agent)
        return agent

    def tearDown(self):
        for agent in self.agents:
            agent.close()
        self.tmp.cleanup()

    def test_same_process_and_thread_retain_more_than_four_turns_and_restart(self):
        agent = self.agent()
        agent.ask('remember persimmon')
        ident, pid = agent.thread_id, agent.process.pid
        for i in range(5):
            agent.ask('filler ' + str(i))
        self.assertEqual(agent.ask('recall'), 'remember persimmon')
        self.assertEqual((agent.thread_id, agent.process.pid), (ident, pid))
        self.assertEqual((self.root / '.state/session.json').stat().st_mode & 0o777, 0o600)
        agent.close()
        resumed = self.agent()
        self.assertEqual(resumed.ask('recall'), 'remember persimmon')
        self.assertEqual(resumed.thread_id, ident)
        self.assertNotEqual(resumed.process.pid, pid)

    def test_instruction_change_applies_after_restart_without_new_conversation(self):
        first = self.agent()
        self.assertEqual(first.ask('instructions'), 'Original instructions')
        ident = first.thread_id
        first.close()
        (self.root / 'AGENTS.md').write_text('Updated instructions')
        second = self.agent()
        self.assertEqual(second.ask('instructions'), 'Updated instructions')
        self.assertEqual(second.thread_id, ident)

    def test_second_server_cannot_own_same_agent(self):
        first = self.agent()
        first.start()
        with self.assertRaisesRegex(RuntimeError, 'Another server'):
            self.agent().start()
        self.assertIsNone(first.process.poll())

    def test_missing_session_does_not_silently_reset_memory(self):
        first = self.agent()
        first.ask('remember persimmon')
        first.close()
        original = (self.root / '.state/session.json').read_bytes()
        (self.root / '.state/fake-history.json').unlink()
        with self.assertRaisesRegex(RuntimeError, 'session not found'):
            self.agent().start()
        self.assertEqual((self.root / '.state/session.json').read_bytes(), original)

    def test_timeout_reaps_process_and_resumes_without_replaying_turn(self):
        agent = self.agent(timeout=1)
        agent.ask('remember persimmon')
        ident = agent.thread_id
        with self.assertRaises(TimeoutError):
            agent.ask('timeout')
        self.assertIsNone(agent.process)
        self.assertEqual(agent.ask('recall'), 'remember persimmon')
        self.assertEqual(agent.thread_id, ident)
        state = json.loads((self.root / '.state/fake-history.json').read_text())
        self.assertEqual(state['messages'].count('timeout'), 1)

    def test_crashed_process_recovers_same_thread(self):
        agent = self.agent()
        agent.ask('remember persimmon')
        ident = agent.thread_id
        os.kill(agent.process.pid, signal.SIGKILL)
        agent.process.wait()
        self.assertEqual(agent.ask('recall'), 'remember persimmon')
        self.assertEqual(agent.thread_id, ident)

    def test_final_phase_only_and_failed_turn_is_not_spoken(self):
        agent = self.agent()
        for prompt, error in [('missing-final', 'no final message'), ('fail', 'test failure')]:
            with self.subTest(prompt=prompt), self.assertRaisesRegex(RuntimeError, error):
                agent.ask(prompt)


class RouterTests(unittest.TestCase):
    """The shared Agent entry point: two backends, two conversations, one directory."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / 'AGENTS.md').write_text('Codex instructions', encoding='utf-8')
        (self.root / 'CLAUDE.md').write_text('Claude instructions', encoding='utf-8')
        self.codex = self.script('fake-codex', FAKE_CODEX)
        self.claude = self.script('fake-claude', FAKE_CLAUDE)
        self.configure('codex')
        self.agents = []

    def script(self, name, body):
        path = self.root / name
        path.write_text('#!' + sys.executable + '\n' + body)
        path.chmod(0o700)
        return str(path)

    def configure(self, kind, codex_effort='none', claude_effort='low'):
        (self.root / 'agent.json').write_text(json.dumps({
            'agent': kind,
            'codex': {'model': 'test-model', 'effort': codex_effort},
            'claude': {'model': 'test-claude', 'effort': claude_effort}}), encoding='utf-8')

    def agent(self, **kwargs):
        agent = Agent(self.root, self.codex, self.claude, **kwargs)
        self.agents.append(agent)
        return agent

    def session(self):
        return json.loads((self.root / '.state/session.json').read_text(encoding='utf-8'))

    def tearDown(self):
        for agent in self.agents:
            agent.close()
        self.tmp.cleanup()

    def test_configuration_migrates_old_shape_and_rejects_bad_values(self):
        self.assertEqual(normalize_config({'model': 'gpt-6-astra', 'effort': 'low'}), {
            'agent': 'codex', 'codex': {'model': 'gpt-6-astra', 'effort': 'low'},
            'claude': {'model': 'sonnet', 'effort': 'low'}})
        for bad in ({'agent': 'gemini'}, {'agent': 'codex', 'codex': {'model': ' '}},
                    {'agent': 'claude', 'claude': {'model': 'sonnet', 'effort': 'ultra'}},
                    {'agent': 'codex', 'codex': {'model': 'x', 'effort': 'nope'}}, []):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                normalize_config(bad)

    def test_claude_conversation_continues_and_resumes_after_restart(self):
        self.configure('claude')
        agent = self.agent()
        agent.ask('remember grape')
        ident = agent.backend.session_id
        self.assertTrue(ident)
        for i in range(3):
            agent.ask('filler ' + str(i))
        self.assertEqual(agent.ask('recall'), 'remember grape')
        self.assertEqual(agent.status()['conversations']['claude'], ident)
        self.assertIsNone(agent.status()['pid'])
        self.assertTrue(agent.status()['running'])
        self.assertEqual((self.root / '.state/session.json').stat().st_mode & 0o777, 0o600)
        agent.close()
        resumed = self.agent()
        self.assertEqual(resumed.ask('recall'), 'remember grape')
        self.assertEqual(resumed.backend.session_id, ident)

    def test_claude_instruction_change_applies_to_the_next_utterance(self):
        self.configure('claude')
        agent = self.agent()
        self.assertEqual(agent.ask('instructions'), 'Claude instructions')
        ident = agent.backend.session_id
        (self.root / 'CLAUDE.md').write_text('Updated instructions', encoding='utf-8')
        self.assertEqual(agent.ask('instructions'), 'Updated instructions')
        self.assertEqual(agent.backend.session_id, ident)

    def test_switching_backends_keeps_two_conversations_and_resumes_each(self):
        agent = self.agent()
        agent.ask('remember codex-apple')
        thread = agent.backend.thread_id
        self.configure('claude')
        agent.reload()
        agent.ask('remember claude-grape')
        session = agent.backend.session_id
        self.assertNotEqual(thread, session)
        self.configure('codex')
        agent.reload()
        self.assertEqual(agent.ask('recall'), 'remember codex-apple')
        self.assertEqual(agent.backend.thread_id, thread)
        self.configure('claude')
        agent.reload()
        self.assertEqual(agent.ask('recall'), 'remember claude-grape')
        self.assertEqual(agent.backend.session_id, session)
        self.assertEqual(self.session(), {'codex': {'thread_id': thread},
                                          'claude': {'session_id': session}})

    def test_legacy_session_and_config_are_migrated_without_losing_the_thread(self):
        agent = self.agent()
        agent.ask('remember codex-apple')
        thread = agent.backend.thread_id
        agent.close()
        (self.root / '.state/session.json').write_text(json.dumps({'thread_id': thread}))
        (self.root / 'agent.json').write_text(json.dumps({'model': 'test-model', 'effort': 'none'}))
        migrated = self.agent()
        self.assertEqual(migrated.ask('recall'), 'remember codex-apple')
        self.assertEqual(migrated.backend.thread_id, thread)
        self.assertEqual(self.session(), {'codex': {'thread_id': thread}})
        self.assertEqual(migrated.status()['agent'], 'codex')

    def test_claude_unknown_session_raises_and_keeps_the_saved_ids(self):
        agent = self.agent()
        agent.ask('remember codex-apple')
        thread = agent.backend.thread_id
        agent.close()
        (self.root / '.state/session.json').write_text(json.dumps(
            {'codex': {'thread_id': thread}, 'claude': {'session_id': 'missing-id'}}))
        original = (self.root / '.state/session.json').read_bytes()
        self.configure('claude')
        broken = self.agent()
        with self.assertRaisesRegex(RuntimeError, 'No conversation found'):
            broken.ask('hello')
        self.assertEqual((self.root / '.state/session.json').read_bytes(), original)
        self.assertEqual(broken.backend.session_id, 'missing-id')

    def test_claude_errors_are_raised_and_never_spoken(self):
        self.configure('claude')
        agent = self.agent()
        for prompt, error in [('missing-final', 'no final message'), ('fail', 'test failure')]:
            with self.subTest(prompt=prompt), self.assertRaisesRegex(RuntimeError, error):
                agent.ask(prompt)

    def test_claude_timeout_reaps_the_process(self):
        self.configure('claude')
        agent = self.agent(timeout=1)
        agent.ask('remember grape')
        ident = agent.backend.session_id
        with self.assertRaises(TimeoutError):
            agent.ask('timeout')
        self.assertIsNone(agent.backend.process)
        self.assertEqual(agent.ask('recall'), 'remember grape')
        self.assertEqual(agent.backend.session_id, ident)

    def test_leading_dash_is_spoken_not_parsed_as_an_option(self):
        self.configure('claude')
        agent = self.agent()
        self.assertEqual(agent.ask('--model evil'), 'reply --model evil')

    def test_reload_waits_for_an_in_flight_ask(self):
        self.configure('claude')
        agent = self.agent(timeout=10)
        finished = []
        worker = threading.Thread(target=lambda: finished.append(agent.ask('slow')))
        worker.start()
        time.sleep(.2)
        started = time.monotonic()
        agent.reload()
        waited = time.monotonic() - started
        worker.join(5)
        self.assertEqual(finished, ['reply slow'])
        self.assertGreater(waited, .4)

    def test_reset_starts_a_new_conversation_and_keeps_the_other(self):
        agent = self.agent()
        agent.ask('remember codex-apple')
        thread = agent.backend.thread_id
        self.configure('claude')
        agent.reload()
        agent.ask('remember claude-grape')
        session = agent.backend.session_id
        status = agent.reset('codex')
        self.assertIsNone(status['conversations']['codex'])
        self.assertEqual(status['conversations']['claude'], session)
        self.assertEqual(self.session(), {'claude': {'session_id': session}})
        self.configure('codex')
        agent.reload()
        agent.ask('remember codex-melon')
        self.assertNotEqual(agent.backend.thread_id, thread)
        self.assertEqual(agent.ask('recall'), 'remember codex-melon')
        self.assertEqual(self.session()['claude'], {'session_id': session})
        with self.assertRaises(ValueError):
            agent.reset('gemini')

    def test_reset_stops_the_running_backend_and_waits_for_an_in_flight_ask(self):
        self.configure('claude')
        agent = self.agent(timeout=10)
        finished = []
        worker = threading.Thread(target=lambda: finished.append(agent.ask('slow')))
        worker.start()
        time.sleep(.2)
        started = time.monotonic()
        agent.reset('claude')
        waited = time.monotonic() - started
        worker.join(5)
        self.assertEqual(finished, ['reply slow'])
        self.assertGreater(waited, .4)
        self.assertIsNone(agent.backend)
        self.assertIsNone(agent.status()['conversations']['claude'])
        agent.ask('remember grape')
        self.assertTrue(agent.backend.session_id)
        self.assertEqual(agent.ask('recall'), 'remember grape')

    def test_reload_reports_a_bad_model_instead_of_leaving_it_running(self):
        agent = self.agent()
        agent.start()
        (self.root / 'agent.json').write_text(json.dumps({'agent': 'codex', 'codex': {'model': ''}}))
        with self.assertRaises(ValueError):
            agent.reload()
        self.assertFalse(agent.status()['running'])
        self.configure('codex')
        agent.reload()
        self.assertTrue(agent.status()['running'])


if __name__ == '__main__':
    unittest.main()
