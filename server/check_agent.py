#!/usr/bin/env python3
"""Opt-in live checks with the real agent, in a throwaway conversation.

Without --image: Codex keeps one process and thread, remembers across a restart, and
picks up changed instructions.

With --image PATH: the chosen backend (--agent codex|claude) is shown the JPEG with the
same fixed question /look uses, then asked about "the photo just now" as plain text, to
confirm the picture stayed in the same conversation. Nothing is synthesised or played.
"""
import argparse
import json
from pathlib import Path
import shutil
import tempfile
import uuid

from stackee_agent import Agent, CodexAgent
from stackee_server import LOOK_PROMPT, clean_reply, read_jpeg

FOLLOW_UP = 'さっきの写真に書いてあった文字と、背景の色を答えてください。'


def prepare(root, template, kind):
    for name in ('AGENTS.md', 'CLAUDE.md'):
        if not (root / name).exists():
            shutil.copyfile(template / name, root / name)
    config = json.loads((template / 'agent.json').read_text(encoding='utf-8'))
    config['agent'] = kind
    (root / 'agent.json').write_text(json.dumps(config, ensure_ascii=False), encoding='utf-8')


def check_image(root, args, template):
    prepare(root, template, args.agent)
    image = read_jpeg(Path(args.image).read_bytes())
    agent = Agent(root, args.codex, args.claude, timeout=args.timeout)
    try:
        agent.start()
        seen = agent.ask(LOOK_PROMPT, image)
        conversation = agent.status()['conversations'][args.agent]
        print(json.dumps({'agent': args.agent, 'image_bytes': len(image), 'conversation': conversation,
                          'reply': seen, 'spoken': clean_reply(seen)}, ensure_ascii=False), flush=True)
        later = agent.ask(FOLLOW_UP)
        assert agent.status()['conversations'][args.agent] == conversation
        print(json.dumps({'follow_up': FOLLOW_UP, 'reply': later, 'same_conversation': True},
                         ensure_ascii=False), flush=True)
    finally:
        agent.close()


def check_memory(root, args, template):
    token = 'みかん' + uuid.uuid4().hex[:8]
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--codex', default='codex')
    parser.add_argument('--claude', default='claude')
    parser.add_argument('--image', help='JPEG to show the agent (the /look check)')
    parser.add_argument('--agent', choices=('codex', 'claude'), default='codex',
                        help='backend for --image')
    parser.add_argument('--agent-dir', type=Path,
                        help='test agent folder to use instead of a temporary one '
                             '(never the production server/agent)')
    parser.add_argument('--timeout', type=float, default=180)
    args = parser.parse_args()
    template = Path(__file__).resolve().parent / 'agent' / 'defaults'
    check = check_image if args.image else check_memory
    if args.agent_dir:
        root = args.agent_dir.resolve()
        if root == template.parent.resolve():
            parser.error('--agent-dir must not be the production agent folder')
        root.mkdir(parents=True, exist_ok=True)
        check(root, args, template)
        return
    with tempfile.TemporaryDirectory(prefix='stackee-agent-check-') as tmp:
        check(Path(tmp), args, template)


if __name__ == '__main__':
    main()
