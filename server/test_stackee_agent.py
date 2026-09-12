import json
import os
from pathlib import Path
import signal
import sys
import tempfile
import unittest

from stackee_agent import CodexAgent


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


if __name__ == '__main__':
    unittest.main()
