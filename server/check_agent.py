#!/usr/bin/env python3
"""Opt-in live Codex check: same process, saved memory, restart, and updated instructions."""
import argparse
import json
from pathlib import Path
import tempfile
import uuid

from stackee_agent import CodexAgent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--codex', default='codex')
    args = parser.parse_args()
    template = Path(__file__).resolve().parent / 'agent'
    token = 'みかん' + uuid.uuid4().hex[:8]
    with tempfile.TemporaryDirectory(prefix='stackee-agent-check-') as tmp:
        root = Path(tmp)
        (root / 'AGENTS.md').write_text((template / 'AGENTS.md').read_text(), encoding='utf-8')
        (root / 'agent.json').write_text((template / 'agent.json').read_text(), encoding='utf-8')
        first = CodexAgent(root, args.codex)
        try:
            first.ask('この会話の合言葉は「' + token + '」です。覚えて、はいとだけ答えてください。')
            ident, pid = first.thread_id, first.process.pid
            reply = first.ask('この会話の合言葉だけを答えてください。')
            assert token in reply, reply
            assert (first.thread_id, first.process.pid) == (ident, pid)
            print(json.dumps({'same_process': True, 'thread_id': ident, 'pid': pid}, ensure_ascii=False), flush=True)
        finally:
            first.close()
        with (root / 'AGENTS.md').open('a', encoding='utf-8') as f:
            f.write('\nすべての返答の末尾に「確認済み」を付けてください。\n')
        second = CodexAgent(root, args.codex)
        try:
            reply = second.ask('この会話の合言葉を答えてください。')
            assert second.thread_id == ident
            assert token in reply, reply
            assert '確認済み' in reply, reply
            print(json.dumps({'resumed_same_thread': True, 'memory_retained': True,
                              'updated_instructions_applied': True, 'pid': second.process.pid}), flush=True)
        finally:
            second.close()


if __name__ == '__main__':
    main()
