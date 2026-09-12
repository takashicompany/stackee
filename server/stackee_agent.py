"""One long-lived Codex app-server and one persisted conversation per agent directory."""
from collections import deque
import fcntl
import json
import logging
import os
from pathlib import Path
import queue
import signal
import subprocess
import threading
import time


class CodexAgent:
    def __init__(self, directory, codex="codex", model=None, timeout=120):
        self.directory = Path(directory).resolve()
        self.codex = codex
        self.model_override = model
        self.timeout = timeout
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
        temporary = self._state.with_suffix(".tmp")
        with open(temporary, "w", encoding="utf-8") as f:
            os.chmod(temporary, 0o600)
            json.dump({"thread_id": self.thread_id}, f)
            f.flush()
            os.fsync(f.fileno())
        temporary.replace(self._state)

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
        config = json.loads((self.directory / "agent.json").read_text(encoding="utf-8"))
        self.model = self.model_override or config["model"]
        self.effort = config["effort"]
        if not instructions.strip() or not isinstance(self.model, str) or not self.model:
            raise ValueError("Agent instructions and model must not be empty")
        if self.effort not in ("none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra"):
            raise ValueError("Invalid agent reasoning effort")
        self._state.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        lock_file = open(self._state.parent / "server.lock", "a")
        try:
            fcntl.flock(lock_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            lock_file.close()
            raise RuntimeError("Another server owns this agent directory") from None
        self._lock_file = lock_file
        self.thread_id = None
        if self._state.exists():
            state = json.loads(self._state.read_text(encoding="utf-8"))
            ident = state.get("thread_id")
            if not isinstance(ident, str) or not ident.strip():
                raise ValueError("Invalid saved agent session; refusing to discard history")
            self.thread_id = ident
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
