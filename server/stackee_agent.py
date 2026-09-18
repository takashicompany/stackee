"""One conversation per backend in an agent directory: a resident Codex app-server, or Claude Code.

The runtime files (AGENTS.md, CLAUDE.md, agent.json, .state/) live in the agent directory and are
not tracked by git, so the admin UI can rewrite them without breaking `git pull --ff-only`.
Missing runtime files are restored from the tracked `defaults/` copies at startup.
"""
from collections import deque
import fcntl
import json
import logging
import os
from pathlib import Path
import queue
import shutil
import signal
import subprocess
import threading
import time

CODEX_EFFORTS = ("none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra")
CLAUDE_EFFORTS = ("low", "medium", "high", "xhigh", "max")
EFFORTS = {"codex": CODEX_EFFORTS, "claude": CLAUDE_EFFORTS}
DEFAULT_MODELS = {"codex": "gpt-6-astra", "claude": "sonnet"}
CONVERSATION_KEYS = {"codex": "thread_id", "claude": "session_id"}
INSTRUCTION_FILES = {"codex": "AGENTS.md", "claude": "CLAUDE.md"}
RUNTIME_FILES = ("AGENTS.md", "CLAUDE.md", "agent.json")


def normalize_config(raw):
    """Validate agent.json and migrate the old {model, effort} shape to the per-backend shape."""
    if not isinstance(raw, dict):
        raise ValueError("Agent configuration must be an object")
    if "agent" not in raw and "model" in raw:
        raw = {"agent": "codex", "codex": {"model": raw.get("model"), "effort": raw.get("effort")}}
    kind = raw.get("agent", "codex")
    if kind not in EFFORTS:
        raise ValueError("Unknown agent kind: " + str(kind))
    config = {"agent": kind}
    for name in ("codex", "claude"):
        section = raw.get(name)
        if section is None:
            section = {}
        if not isinstance(section, dict):
            raise ValueError("Agent settings must be an object: " + name)
        model = section.get("model", DEFAULT_MODELS[name])
        effort = section.get("effort", "low")
        if not isinstance(model, str) or not model.strip():
            raise ValueError("Agent model must not be empty: " + name)
        if effort not in EFFORTS[name]:
            raise ValueError("Invalid reasoning effort for " + name + ": " + str(effort))
        config[name] = {"model": model.strip(), "effort": effort}
    return config


def read_config(path):
    return normalize_config(json.loads(Path(path).read_text(encoding="utf-8")))


def write_atomic(path, text, mode=0o644):
    path = Path(path)
    temporary = path.parent / (path.name + ".tmp")
    with open(temporary, "w", encoding="utf-8") as f:
        os.chmod(temporary, mode)
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    temporary.replace(path)


def load_session(path):
    """Read .state/session.json, migrating the old {thread_id} shape to {codex: {thread_id}}."""
    path = Path(path)
    if not path.exists():
        return {}
    state = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(state, dict):
        raise ValueError("Invalid saved agent session; refusing to discard history")
    if "thread_id" in state:
        return {"codex": {"thread_id": state["thread_id"]}}
    return state


def read_conversation(path, kind):
    section = load_session(path).get(kind)
    if not isinstance(section, dict):
        if section is None:
            return None
        raise ValueError("Invalid saved agent session; refusing to discard history")
    ident = section.get(CONVERSATION_KEYS[kind])
    if ident is None:
        return None
    if not isinstance(ident, str) or not ident.strip():
        raise ValueError("Invalid saved agent session; refusing to discard history")
    return ident


def save_conversation(path, kind, ident):
    """Keep the other backend's conversation; never rewrite the file from memory alone."""
    path = Path(path)
    state = load_session(path)
    section = dict(state.get(kind) or {})
    section[CONVERSATION_KEYS[kind]] = ident
    state[kind] = section
    temporary = path.parent / (path.name + ".tmp")
    with open(temporary, "w", encoding="utf-8") as f:
        os.chmod(temporary, 0o600)
        json.dump(state, f)
        f.flush()
        os.fsync(f.fileno())
    temporary.replace(path)


def ensure_runtime_files(directory):
    """Restore the untracked runtime files from the tracked defaults, without touching edits."""
    directory = Path(directory)
    for name in RUNTIME_FILES:
        target = directory / name
        source = directory / "defaults" / name
        if not target.exists() and source.exists():
            shutil.copyfile(source, target)


def _acquire(state_file):
    state_file.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    lock_file = open(state_file.parent / "server.lock", "a")
    try:
        fcntl.flock(lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        lock_file.close()
        raise RuntimeError("Another server owns this agent directory") from None
    return lock_file


class CodexAgent:
    kind = "codex"

    def __init__(self, directory, codex="codex", model=None, timeout=120, settings=None):
        self.directory = Path(directory).resolve()
        self.codex = codex
        self.model_override = model
        self.timeout = timeout
        self.settings = settings
        self.process = None
        self.thread_id = None
        self.model = None
        self.effort = None
        self.closed = False
        self._lock_file = None
        self._serial = threading.Lock()
        self._lifecycle = threading.Lock()
        self._counter = 0
        self._pending = deque()
        self._messages = queue.Queue()
        self._reader = None
        self._state = self.directory / ".state" / "session.json"

    def _send(self, message):
        if self.closed or not self.process or self.process.poll() is not None:
            raise RuntimeError("Codex agent is not running")
        self.process.stdin.write(json.dumps(message, ensure_ascii=False) + "\n")
        self.process.stdin.flush()

    @staticmethod
    def _read_output(stream, messages):
        try:
            for line in stream:
                messages.put(json.loads(line))
        except Exception as exc:
            messages.put(exc)
        finally:
            messages.put(None)

    def _receive(self, deadline):
        while True:
            try:
                message = self._messages.get(timeout=max(0, deadline - time.monotonic()))
            except queue.Empty:
                raise TimeoutError("Codex agent timed out") from None
            if message is None:
                raise RuntimeError("Codex agent disconnected; conversation is retained")
            if isinstance(message, Exception):
                raise RuntimeError("Invalid Codex protocol output") from message
            if "method" in message and "id" in message:
                # This voice client has no interactive approval/form UI.
                self._send({"id": message["id"], "error": {
                    "code": -32601, "message": "Unsupported request in voice client"}})
                continue
            return message

    def _request(self, method, params, deadline):
        self._counter += 1
        ident = self._counter
        self._send({"id": ident, "method": method, "params": params})
        while True:
            message = self._receive(deadline)
            if message.get("id") == ident:
                if "error" in message:
                    raise RuntimeError("Codex " + method + ": " +
                                       message["error"].get("message", "request failed"))
                return message["result"]
            if "method" in message:
                self._pending.append(message)

    def _save_session(self):
        save_conversation(self._state, self.kind, self.thread_id)

    def start(self):
        with self._serial:
            try:
                self._start()
            except BaseException:
                self._stop_process()
                raise

    def _start(self):
        if self.closed:
            raise RuntimeError("Codex agent is closed")
        if self.process and self.process.poll() is None:
            return
        self._stop_process()
        instructions = (self.directory / "AGENTS.md").read_text(encoding="utf-8")
        settings = self.settings or read_config(self.directory / "agent.json")["codex"]
        self.model = self.model_override or settings["model"]
        self.effort = settings["effort"]
        if not instructions.strip() or not isinstance(self.model, str) or not self.model:
            raise ValueError("Agent instructions and model must not be empty")
        if self.effort not in CODEX_EFFORTS:
            raise ValueError("Invalid agent reasoning effort")
        if not shutil.which(self.codex):
            raise RuntimeError("Executable not found: " + self.codex)
        self._lock_file = _acquire(self._state)
        self.thread_id = read_conversation(self._state, self.kind)
        self._messages = queue.Queue()
        self._pending.clear()
        with self._lifecycle:
            if self.closed:
                raise RuntimeError("Codex agent is closed")
            self.process = subprocess.Popen(
                [self.codex, "app-server", "--listen", "stdio://"],
                cwd=self.directory, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                text=True, encoding="utf-8", bufsize=1, start_new_session=True)
        self._reader = threading.Thread(target=self._read_output,
                                        args=(self.process.stdout, self._messages), daemon=True)
        self._reader.start()
        deadline = time.monotonic() + self.timeout
        self._request("initialize", {"clientInfo": {
            "name": "stackee_voice", "title": "stackee voice", "version": "1.0.0"}}, deadline)
        self._send({"method": "initialized", "params": {}})
        params = {"cwd": str(self.directory), "model": self.model,
                  "approvalPolicy": "never", "sandbox": "read-only",
                  "developerInstructions": instructions,
                  "config": {"model_reasoning_effort": self.effort}}
        if self.thread_id:
            params.update(threadId=self.thread_id, excludeTurns=True)
            result = self._request("thread/resume", params, deadline)
            if result["thread"]["id"] != self.thread_id:
                raise RuntimeError("Codex resumed a different conversation")
        else:
            result = self._request("thread/start", dict(params, ephemeral=False), deadline)
            self.thread_id = result["thread"]["id"]
        self.model = result.get("model", self.model)
        logging.info("Codex agent pid=%s cwd=%s thread=%s model=%s effort=%s",
                     self.process.pid, self.directory, self.thread_id, self.model, self.effort)

    def ask(self, text):
        with self._serial:
            try:
                self._start()
                self._pending.clear()
                deadline = time.monotonic() + self.timeout
                result = self._request("turn/start", {
                    "threadId": self.thread_id, "cwd": str(self.directory),
                    "model": self.model, "effort": self.effort,
                    "input": [{"type": "text", "text": text}]}, deadline)
                turn_id = result["turn"]["id"]
                # Zero-turn threads may not have a rollout yet; save after a turn is accepted.
                self._save_session()
                final = ""
                while True:
                    event = self._pending.popleft() if self._pending else self._receive(deadline)
                    params = event.get("params", {})
                    if params.get("threadId") != self.thread_id:
                        continue
                    if event.get("method") == "item/completed" and params.get("turnId") == turn_id:
                        item = params["item"]
                        if item.get("type") == "agentMessage" and item.get("phase") in (None, "final_answer"):
                            final = item.get("text", "").strip()
                    if event.get("method") == "turn/completed" and params["turn"]["id"] == turn_id:
                        turn = params["turn"]
                        if turn["status"] != "completed":
                            error = turn.get("error") or {}
                            raise RuntimeError("Codex turn " + turn["status"] + ": " + error.get("message", ""))
                        if not final:
                            raise RuntimeError("codex returned no final message")
                        return final
            except BaseException:
                # Never automatically replay an accepted user turn or create a new thread.
                self._stop_process()
                raise

    def running(self):
        proc = self.process
        return bool(proc and proc.poll() is None)

    def _stop_process(self):
        with self._lifecycle:
            proc, self.process = self.process, None
        if proc:
            if proc.poll() is None:
                try:
                    os.killpg(proc.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait(timeout=3)
            if self._reader:
                self._reader.join(timeout=1)
            proc.stdin.close()
            proc.stdout.close()
        if self._lock_file:
            self._lock_file.close()
            self._lock_file = None

    def close(self):
        self.closed = True
        self._stop_process()

    def status(self):
        proc = self.process
        return {"thread_id": self.thread_id, "pid": proc.pid if proc and proc.poll() is None else None,
                "model": self.model, "effort": self.effort, "cwd": str(self.directory)}


class ClaudeAgent:
    """One `claude -p` process per utterance, resuming the saved session id each time."""
    kind = "claude"

    def __init__(self, directory, claude="claude", timeout=120, settings=None):
        self.directory = Path(directory).resolve()
        self.claude = claude
        self.timeout = timeout
        self.settings = settings
        self.session_id = None
        self.model = None
        self.effort = None
        self.ready = False
        self.closed = False
        self.process = None
        self._lock_file = None
        self._serial = threading.Lock()
        self._lifecycle = threading.Lock()
        self._state = self.directory / ".state" / "session.json"

    def start(self):
        with self._serial:
            try:
                self._start()
            except BaseException:
                self._release()
                raise

    def _start(self):
        if self.closed:
            raise RuntimeError("Claude agent is closed")
        if self.ready:
            return
        instructions = (self.directory / "CLAUDE.md").read_text(encoding="utf-8")
        settings = self.settings or read_config(self.directory / "agent.json")["claude"]
        model = settings["model"]
        effort = settings["effort"]
        if not instructions.strip() or not isinstance(model, str) or not model:
            raise ValueError("Agent instructions and model must not be empty")
        if effort not in CLAUDE_EFFORTS:
            raise ValueError("Invalid agent reasoning effort")
        if not shutil.which(self.claude):
            raise RuntimeError("Executable not found: " + self.claude)
        self._lock_file = _acquire(self._state)
        self.session_id = read_conversation(self._state, self.kind)
        self.model, self.effort = model, effort
        self.ready = True
        logging.info("Claude agent cwd=%s session=%s model=%s effort=%s",
                     self.directory, self.session_id, self.model, self.effort)

    def _argv(self):
        argv = [self.claude, "-p", "--output-format", "json", "--model", self.model,
                "--effort", self.effort, "--tools", "", "--strict-mcp-config",
                "--setting-sources", "project", "--permission-mode", "plan"]
        if self.session_id:
            argv += ["--resume", self.session_id]
        return argv

    def ask(self, text):
        with self._serial:
            self._start()
            # The utterance goes in on stdin: a leading "-" must never be read as an option.
            with self._lifecycle:
                if self.closed:
                    raise RuntimeError("Claude agent is closed")
                proc = subprocess.Popen(self._argv(), cwd=self.directory,
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, start_new_session=True)
                self.process = proc
            try:
                try:
                    out, err = proc.communicate(input=text.encode("utf-8"), timeout=self.timeout)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(proc.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    proc.communicate()
                    raise TimeoutError("Claude agent timed out") from None
                return self._reply(out, err)
            finally:
                with self._lifecycle:
                    self.process = None

    def _reply(self, out, err):
        if not out.strip():
            detail = err.decode("utf-8", "replace").strip() or "no output"
            raise RuntimeError("claude failed: " + detail[-500:])
        try:
            data = json.loads(out.decode("utf-8", "replace"))
        except json.JSONDecodeError as exc:
            raise RuntimeError("Invalid claude output") from exc
        ident = data.get("session_id")
        if isinstance(ident, str) and ident.strip():
            if self.session_id and ident != self.session_id:
                raise RuntimeError("Claude resumed a different conversation")
            if not self.session_id:
                self.session_id = ident
                save_conversation(self._state, self.kind, ident)
        if data.get("is_error") or data.get("subtype") != "success":
            detail = str(data.get("result") or data.get("subtype") or "request failed")
            raise RuntimeError("claude: " + detail.strip()[:500])
        if not self.session_id:
            raise RuntimeError("claude returned no session id")
        reply = (data.get("result") or "").strip()
        if not reply:
            raise RuntimeError("claude returned no final message")
        return reply

    def running(self):
        return self.ready and not self.closed

    def _release(self):
        with self._lifecycle:
            proc, self.process = self.process, None
        if proc and proc.poll() is None:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.communicate()
        self.ready = False
        if self._lock_file:
            self._lock_file.close()
            self._lock_file = None

    def close(self):
        self.closed = True
        self._release()

    def status(self):
        return {"session_id": self.session_id, "pid": None, "model": self.model,
                "effort": self.effort, "cwd": str(self.directory)}


class Agent:
    """The single entry point the voice pipeline uses; routes to the configured backend."""

    def __init__(self, directory, codex="codex", claude="claude", codex_model=None, timeout=120):
        self.directory = Path(directory).resolve()
        self.codex = codex
        self.claude = claude
        self.codex_model = codex_model
        self.timeout = timeout
        self.closed = False
        self.config = None
        self.backend = None
        self._serial = threading.Lock()
        self._state = self.directory / ".state" / "session.json"

    def _ensure(self):
        if self.closed:
            raise RuntimeError("Agent is closed")
        if self.config is None:
            self.config = read_config(self.directory / "agent.json")
        kind = self.config["agent"]
        if self.backend is not None and self.backend.kind != kind:
            self.backend.close()
            self.backend = None
        if self.backend is None:
            settings = self.config[kind]
            if kind == "codex":
                self.backend = CodexAgent(self.directory, self.codex, self.codex_model,
                                          self.timeout, settings)
            else:
                self.backend = ClaudeAgent(self.directory, self.claude, self.timeout, settings)
        return self.backend

    def start(self):
        with self._serial:
            ensure_runtime_files(self.directory)
            self._ensure().start()

    def ask(self, text):
        with self._serial:
            return self._ensure().ask(text)

    def reload(self):
        """Stop the running backend and re-read agent.json; raise if the new settings are wrong."""
        with self._serial:
            backend, self.backend = self.backend, None
            if backend is not None:
                backend.close()
            self.config = None
            ensure_runtime_files(self.directory)
            self._ensure().start()
        return self.status()

    def close(self):
        self.closed = True
        backend = self.backend
        if backend is not None:
            backend.close()

    def status(self):
        backend = self.backend
        try:
            state = load_session(self._state)
        except (OSError, ValueError, json.JSONDecodeError):
            state = {}
        conversations = {kind: (state.get(kind) or {}).get(key)
                         for kind, key in CONVERSATION_KEYS.items()}
        info = backend.status() if backend is not None else {}
        if backend is not None:
            live = info.get(CONVERSATION_KEYS[backend.kind])
            if live:
                conversations[backend.kind] = live
        return {"agent": (self.config or {}).get("agent"),
                "running": bool(backend is not None and backend.running()),
                "pid": info.get("pid"), "model": info.get("model"), "effort": info.get("effort"),
                "cwd": str(self.directory), "conversations": conversations}
