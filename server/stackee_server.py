#!/usr/bin/env python3
"""LAN voice receiver: WAV → whisper-cli → the configured agent → say or VOICEVOX.

Python standard library only. POST /talk accepts WAV; poll the returned Location
for status, then GET its /audio endpoint for 16 kHz, signed little-endian PCM.
GET /admin serves the browser page that switches agent, model, effort and instructions.
"""
from __future__ import annotations

import argparse
import array
from collections import deque
import io
import json
import logging
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import uuid
import urllib.parse
import urllib.request
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import stackee_agent
from stackee_agent import Agent

RATE = 16000
MAX_SECONDS = 30
MAX_UPLOAD = RATE * 2 * MAX_SECONDS + 4096
# Recording is capped at MAX_SECONDS; the spoken reply may run much longer.
MAX_REPLY_SECONDS = 120
MAX_REPLY_BYTES = RATE * 2 * MAX_REPLY_SECONDS
MAX_ADMIN_BYTES = 64 * 1024
AGENT_DIR = Path(__file__).resolve().parent / "agent"
ADMIN_HTML = Path(__file__).resolve().parent / "admin.html"


def read_audio(data, max_seconds=MAX_SECONDS, min_seconds=.3):
    try:
        with wave.open(io.BytesIO(data), "rb") as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(),
                    wav.getcomptype()) != (1, 2, RATE, "NONE"):
                raise ValueError("Expected uncompressed 16000 Hz / 16-bit / mono WAV")
            frames = wav.getnframes()
            if not int(RATE * min_seconds) <= frames <= RATE * max_seconds:
                raise ValueError("Audio must be between " + str(min_seconds) + " and "
                                 + str(max_seconds) + " seconds")
            pcm = wav.readframes(frames)
            if len(pcm) != frames * 2:
                raise ValueError("Truncated WAV")
            return pcm
    except (wave.Error, EOFError) as exc:
        raise ValueError("Invalid WAV") from exc


def peak(pcm):
    samples = array.array("h", pcm)
    if sys.byteorder != "little":
        samples.byteswap()
    return max((abs(x) for x in samples), default=0)


def limit_speaker_peak(pcm, limit=0.25):
    """Match the existing CoreS3 speaker level; never amplify quiet speech."""
    highest = peak(pcm)
    target = int(32767 * limit)
    if highest <= target:
        return pcm
    samples = array.array("h", pcm)
    if sys.byteorder != "little":
        samples.byteswap()
    scale = target / highest
    for i, value in enumerate(samples):
        samples[i] = int(value * scale)
    if sys.byteorder != "little":
        samples.byteswap()
    return samples.tobytes()


def clean_transcript(text):
    text = re.sub(r"[\[（(][^\]）)\n]{0,40}[\]）)]", "", text)
    text = " ".join(text.split()).strip()
    if text.strip("。、. ") in ("ご視聴ありがとうございました", "ありがとうございました",
                                   "チャンネル登録お願いします", "Thank you for watching"):
        return ""
    return text


_URL_TAIL = r"(?:[^\s<>\[\]{}\"'`。、！？「」『』（）()]|\([^()\s]*\))"
_URL = re.compile(
    r"(?i)(?:[a-z][a-z0-9+.-]*://|www\.)" + _URL_TAIL + r"+"
    r"|(?<![a-z0-9_@])//[a-z0-9]" + _URL_TAIL + r"*"
    r"|(?<![a-z0-9_@])(?:[a-z0-9](?:[a-z0-9-]*[a-z0-9])?\.)+"
    r"[a-z]{2,63}(?![a-z0-9_-])(?::\d+)?"
    r"(?:[/?#]" + _URL_TAIL + r"*)?"
)


def clean_reply(text):
    """Keep link labels as speech, remove destinations before truncating the reply."""
    text = re.sub(r"[^]*", "", text)
    text = re.sub(r"(?m)^\s{0,3}\[[^\]\n]+\]:[^\n]*(?:\n|$)", "", text)
    text = re.sub(r"!?\[([^\]\n]*)\]\(\s*(?:[^()\n]|\([^()\n]*\))*\)", r"\1", text)
    text = re.sub(r"\[([^\]\n]+)\]\[[^\]\n]*\]", r"\1", text)
    text = _URL.sub("", text)
    text = re.sub(r"\(\s*\)|\[\s*\]|<\s*>|（\s*）|「\s*」|`+", "", text)
    text = " ".join(text.split()).strip()
    text = re.sub(r"\s+([。、！？])", r"\1", text)
    if not re.search(r"\w", text):
        return "すみません。回答をうまくまとめられませんでした。"
    return text


SENTENCE_MARKS = "。．！？!?\n"
CLAUSE_MARKS = "、，,"
SILENCE = b"\0\0" * (RATE * 150 // 1000)   # a breath between sentences


def split_parts(text, marks):
    """Split after each mark, keeping it, so the pieces rejoin into the original text."""
    parts, current = [], ""
    for character in text:
        current += character
        if character in marks:
            parts.append(current)
            current = ""
    parts.append(current)
    return [part for part in parts if part.strip()]


class Pipeline:
    def __init__(self, model, whisper="whisper-cli", codex="codex", codex_model=None,
                 voice="Kyoko", timeout=120, echo=False, tts="say",
                 voicevox_url="http://127.0.0.1:50021", speaker=3, agent_dir=None,
                 claude="claude"):
        self.model = str(Path(model).resolve())
        self.whisper = whisper
        self.codex = codex
        self.codex_model = codex_model
        self.voice = voice
        self.timeout = timeout
        self.echo = echo
        if tts not in ("say", "voicevox"):
            raise ValueError("Unknown TTS backend: " + tts)
        self.tts = tts
        self.voicevox_url = voicevox_url.rstrip("/")
        self.speaker = speaker
        self.failure = None
        self.claude = claude
        self.agent = None if echo else Agent(agent_dir or AGENT_DIR, codex, claude,
                                             codex_model, timeout)
        self.lock = threading.Lock()
        self.process = None
        self.closed = False

    def check(self):
        for cmd in (self.whisper,) + (("say",) if self.tts == "say" else ()):
            if not shutil.which(cmd):
                raise RuntimeError("Executable not found: " + cmd)
        if not Path(self.model).is_file():
            raise RuntimeError("Whisper model not found: " + self.model)
        if self.tts == "voicevox":
            speakers = json.loads(self.voicevox_request("/speakers"))
            if not any(style["id"] == self.speaker for speaker in speakers
                       for style in speaker["styles"]):
                raise RuntimeError("VOICEVOX speaker not found: " + str(self.speaker))
            self.voicevox_request("/initialize_speaker?" + urllib.parse.urlencode(
                {"speaker": self.speaker, "skip_reinit": "true"}), b"")
        if self.agent:
            self.agent.start()

    def voicevox_request(self, path, data=None):
        request = urllib.request.Request(self.voicevox_url + path, data=data,
                                         headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(request, timeout=self.timeout) as response:
            body = response.read(MAX_REPLY_BYTES + 4097)
        if len(body) > MAX_REPLY_BYTES + 4096:
            raise RuntimeError("VOICEVOX response exceeds device limit")
        return body

    def synthesize(self, reply, root):
        if self.tts == "voicevox":
            params = urllib.parse.urlencode({"text": reply, "speaker": self.speaker})
            query = json.loads(self.voicevox_request("/audio_query?" + params, b""))
            query.update(outputSamplingRate=RATE, outputStereo=False)
            speech = self.voicevox_request("/synthesis?speaker=" + str(self.speaker),
                                          json.dumps(query).encode("utf-8"))
        else:
            path = root / "speech.wav"
            self.run(["say", "-v", self.voice, "-o", str(path), "--file-format=WAVE",
                      "--data-format=LEI16@16000", "--channels=1", "--", reply], str(root))
            speech = path.read_bytes()
        return read_audio(speech, MAX_REPLY_SECONDS, .02)

    def fit(self, reply, root, limit=MAX_REPLY_BYTES):
        """Speak one sentence at a time and stop just before the device buffer would overflow.

        Synthesising the whole reply at once fails on long texts (VOICEVOX answers 500) and
        inflates the engine's GPU arena, so each sentence is its own request and the pieces
        are joined with a short silence. A sentence the engine refuses, or the opening
        sentence when it alone is too long, is retried clause by clause.
        """
        self.failure = None
        chunks, kept, used = [], "", 0
        units = deque(split_parts(reply, SENTENCE_MARKS))
        while units:
            unit = units.popleft()
            audio = self.speak(unit, root)
            clauses = split_parts(unit, CLAUSE_MARKS)
            if audio is None:
                if len(clauses) < 2:
                    break
                units.extendleft(reversed(clauses))
                continue
            gap = SILENCE if chunks else b""
            if used + len(gap) + len(audio) > limit:
                if not chunks and len(clauses) > 1:
                    units.extendleft(reversed(clauses))
                    continue
                break
            chunks.append(gap)
            chunks.append(audio)
            used += len(gap) + len(audio)
            kept += unit
        kept = kept.strip().rstrip(CLAUSE_MARKS)
        if not kept or not used:
            if self.failure is not None:
                raise self.failure
            raise RuntimeError("Reply audio exceeds device limit")
        if len(kept) < len(reply.strip()):
            logging.info("reply-shortened original=%d kept=%d bytes=%d limit=%d",
                         len(reply.strip()), len(kept), used, limit)
        return kept, limit_speaker_peak(b"".join(chunks))

    def speak(self, text, root):
        """PCM for one piece of text, or None when the engine refuses it."""
        try:
            return self.synthesize(text, root)
        except (OSError, RuntimeError, ValueError) as exc:
            self.failure = exc
            logging.warning("Synthesis failed for %d characters: %s", len(text), exc)
            return None

    def run(self, argv, cwd, input=None):
        with self.lock:
            if self.closed:
                raise RuntimeError("Server is stopping")
            proc = subprocess.Popen(argv, cwd=cwd, stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    start_new_session=True)
            self.process = proc
        try:
            try:
                out, err = proc.communicate(input=input, timeout=self.timeout)
            except subprocess.TimeoutExpired:
                self.kill(proc)
                proc.communicate()
                raise RuntimeError(Path(argv[0]).name + " timed out")
            if proc.returncode:
                logging.error("%s failed: %s", Path(argv[0]).name,
                              err.decode("utf-8", "replace")[-1000:])
                raise RuntimeError(Path(argv[0]).name + " failed")
            return out.decode("utf-8", "replace").strip()
        finally:
            with self.lock:
                self.process = None

    @staticmethod
    def kill(proc):
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass

    def close(self):
        with self.lock:
            self.closed = True
            if self.process:
                self.kill(self.process)
        if self.agent:
            self.agent.close()

    def __call__(self, data):
        started = time.monotonic()
        timings = {}
        pcm = read_audio(data)
        if peak(pcm) < 150:
            return {"state": "ignored", "reason": "silence"}, b""
        with tempfile.TemporaryDirectory(prefix="stackee-talk-") as tmp:
            root = Path(tmp)
            wav = root / "input.wav"
            wav.write_bytes(data)
            stage = time.monotonic()
            timings["prepare_ms"] = round((stage - started) * 1000, 2)
            text = clean_transcript(self.run(
                [self.whisper, "-m", self.model, "-f", str(wav), "-l", "ja", "-nt", "-np"], tmp))
            timings["stt_ms"] = round((time.monotonic() - stage) * 1000, 2)
            if not text:
                return {"state": "ignored", "reason": "no_speech"}, b""
            stage = time.monotonic()
            if self.echo:
                reply = text
            else:
                reply = self.agent.ask(text)
                if not reply:
                    raise RuntimeError("codex returned no final message")
            reply = clean_reply(reply)
            timings["codex_ms"] = round((time.monotonic() - stage) * 1000, 2)
            stage = time.monotonic()
            # Keep synthesis and device memory bounded even when the model ignores brevity.
            reply, audio = self.fit(reply, root)
            timings["tts_ms"] = round((time.monotonic() - stage) * 1000, 2)
            timings["total_ms"] = round((time.monotonic() - started) * 1000, 2)
            return {"state": "done", "transcript": text, "reply": reply, "timings": timings}, audio


class Jobs:
    """One conversation at a time; results expire after five minutes, max 8 retained."""
    def __init__(self, pipeline):
        self.pipeline = pipeline
        self.lock = threading.Lock()
        self.entries = {}
        self.busy = False
        self.worker = None

    def prune(self):
        for key in list(self.entries):
            if (self.entries[key]["state"] != "processing"
                    and time.monotonic() - self.entries[key]["created"] > 300):
                del self.entries[key]

    def submit(self, data):
        with self.lock:
            if self.busy:
                return None
            self.prune()
            while len(self.entries) >= 8:
                del self.entries[next(iter(self.entries))]
            ident = uuid.uuid4().hex
            self.entries[ident] = {"created": time.monotonic(), "state": "processing"}
            self.busy = True
            self.worker = threading.Thread(target=self.process, args=(ident, data), daemon=True)
            self.worker.start()
            return ident

    def process(self, ident, data):
        started = time.monotonic()
        try:
            result, audio = self.pipeline(data)
            result["audio"] = audio
        except Exception as exc:
            logging.exception("Voice job %s failed", ident)
            result = {"state": "error", "error": str(exc)}
        logging.info("job-timing %s", json.dumps({"job": ident, "state": result["state"],
                     "worker_ms": round((time.monotonic() - started) * 1000, 2),
                     "timings": result.get("timings", {})}))
        with self.lock:
            if ident in self.entries:
                self.entries[ident].update(result)
                self.entries[ident]["created"] = time.monotonic()
            self.busy = False

    def get(self, ident):
        with self.lock:
            self.prune()
            item = self.entries.get(ident)
            return dict(item) if item else None


class Handler(BaseHTTPRequestHandler):
    # HTTP/1.0 closes each request, so malformed/unread bodies cannot be reused.
    def setup(self):
        super().setup()
        self.connection.settimeout(15)

    def log_message(self, fmt, *args):
        logging.info(fmt, *args)

    def send(self, status, body, content_type="application/json", location=None):
        if isinstance(body, dict):
            body = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        if location:
            self.send_header("Location", location)
        self.end_headers()
        self.wfile.write(body)

    @property
    def agent(self):
        return getattr(self.server.jobs.pipeline, "agent", None)

    def admin_allowed(self):
        """Browsers may read /admin, but only the page itself may write, and only same-origin."""
        if self.headers.get("X-Stackee-Admin") != "1":
            self.send(403, {"error": "admin_header_required"})
            return False
        origin = self.headers.get("Origin")
        if origin and urllib.parse.urlsplit(origin).netloc != (self.headers.get("Host") or ""):
            self.send(403, {"error": "cross_origin"})
            return False
        return True

    def admin_body(self):
        lengths = self.headers.get_all("Content-Length", [])
        if self.headers.get("Transfer-Encoding") or len(lengths) != 1 or not lengths[0].isdigit():
            self.send(411, {"error": "content_length_required"})
            return None
        length = int(lengths[0])
        if length > MAX_ADMIN_BYTES:
            self.send(413, {"error": "too_large"})
            return None
        data = self.rfile.read(length)
        if len(data) != length:
            self.send(400, {"error": "incomplete_body"})
            return None
        return data

    def admin_write(self, path):
        agent = self.agent
        if agent is None:
            return self.send(503, {"error": "agent_disabled"})
        if not self.admin_allowed():
            return None
        data = self.admin_body()
        if data is None:
            return None
        try:
            if path == "/admin/api/config":
                config = stackee_agent.normalize_config(json.loads(data.decode("utf-8")))
                stackee_agent.write_atomic(agent.directory / "agent.json",
                                           json.dumps(config, ensure_ascii=False, indent=2) + "\n")
                return self.send(200, {"saved": True, "config": config})
            kind = path.rsplit("/", 1)[1]
            text = data.decode("utf-8")
            if not text.strip():
                return self.send(400, {"error": "instructions_empty"})
            stackee_agent.write_atomic(agent.directory / stackee_agent.INSTRUCTION_FILES[kind],
                                       text.replace("\r\n", "\n"))
            return self.send(200, {"saved": True})
        except (ValueError, UnicodeDecodeError) as exc:
            return self.send(400, {"error": str(exc)})
        except OSError as exc:
            return self.send(500, {"error": str(exc)})

    def admin_state(self):
        agent = self.agent
        if agent is None:
            return self.send(503, {"error": "agent_disabled"})
        instructions = {}
        for kind, name in stackee_agent.INSTRUCTION_FILES.items():
            path = agent.directory / name
            instructions[kind] = path.read_text(encoding="utf-8") if path.exists() else ""
        try:
            config = stackee_agent.read_config(agent.directory / "agent.json")
        except (OSError, ValueError) as exc:
            return self.send(500, {"error": str(exc)})
        return self.send(200, {"config": config, "instructions": instructions,
                               "status": agent.status(), "busy": self.server.jobs.busy,
                               "choices": {"codex_effort": list(stackee_agent.CODEX_EFFORTS),
                                           "claude_effort": list(stackee_agent.CLAUDE_EFFORTS)}})

    def do_GET(self):
        if self.path == "/health":
            agent = self.agent
            status = agent.status() if agent else None
            return self.send(200, {"ok": True, "agent": (status or {}).get("agent") or "echo",
                                   "busy": self.server.jobs.busy, "conversation": status})
        if self.path == "/admin":
            if not ADMIN_HTML.is_file():
                return self.send(404, {"error": "not_found"})
            return self.send(200, ADMIN_HTML.read_bytes(), "text/html; charset=utf-8")
        if self.path == "/admin/api/state":
            return self.admin_state()
        match = re.fullmatch(r"/jobs/([0-9a-f]{32})(/audio)?", self.path)
        item = self.server.jobs.get(match[1]) if match else None
        if not item:
            return self.send(404, {"error": "not_found"})
        audio = item.pop("audio", b"")
        item.pop("created")
        if match[2]:
            if item["state"] != "done":
                return self.send(409, {"error": "audio_not_ready"})
            return self.send(200, audio, "application/octet-stream")
        if item["state"] == "done":
            item.update(audio_url=self.path + "/audio", sample_rate=RATE,
                        channels=1, sample_width=2, audio_bytes=len(audio))
        return self.send(200, item)

    def do_PUT(self):
        if self.path not in ("/admin/api/config", "/admin/api/instructions/codex",
                             "/admin/api/instructions/claude"):
            return self.send(404, {"error": "not_found"})
        return self.admin_write(self.path)

    def admin_action(self, call, extra):
        agent = self.agent
        if agent is None:
            return self.send(503, {"error": "agent_disabled"})
        if not self.admin_allowed():
            return None
        if self.admin_body() is None:
            return None
        try:
            status = call(agent)
        except Exception as exc:
            logging.exception("Admin action failed")
            return self.send(500, {"error": str(exc) or exc.__class__.__name__})
        return self.send(200, dict(extra, status=status))

    def do_POST(self):
        if self.path == "/admin/api/apply":
            return self.admin_action(lambda agent: agent.reload(), {"applied": True})
        reset = re.fullmatch(r"/admin/api/conversation/(codex|claude)/reset", self.path)
        if reset:
            return self.admin_action(lambda agent: agent.reset(reset[1]),
                                     {"reset": reset[1]})
        if self.path.startswith("/admin/"):
            return self.send(404, {"error": "not_found"})
        if self.path != "/talk":
            return self.send(404, {"error": "not_found"})
        # Device API only. No CORS; reject browser-originated posts explicitly.
        if self.headers.get("Origin"):
            return self.send(403, {"error": "device_api_only"})
        if self.headers.get("Transfer-Encoding"):
            return self.send(400, {"error": "content_length_required"})
        if self.headers.get_content_type() not in ("audio/wav", "audio/x-wav"):
            return self.send(415, {"error": "expected_audio_wav"})
        lengths = self.headers.get_all("Content-Length", [])
        if len(lengths) != 1 or not lengths[0].isascii() or not lengths[0].isdigit():
            return self.send(411, {"error": "content_length_required"})
        length = int(lengths[0])
        if not 44 <= length <= MAX_UPLOAD:
            return self.send(413, {"error": "recording_too_large_or_empty"})
        try:
            data = self.rfile.read(length)
            if len(data) != length:
                raise ValueError("Incomplete request body")
            read_audio(data)
        except TimeoutError:
            return self.send(408, {"error": "upload_timeout"})
        except ValueError as exc:
            return self.send(400, {"error": str(exc)})
        ident = self.server.jobs.submit(data)
        if ident is None:
            return self.send(409, {"error": "busy"})
        location = "/jobs/" + ident
        self.send(202, {"id": ident, "status_url": location}, location=location)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8766)
    parser.add_argument("--whisper-model", required=True)
    parser.add_argument("--whisper", default="whisper-cli")
    parser.add_argument("--codex", default="codex")
    parser.add_argument("--codex-model")
    parser.add_argument("--claude", default="claude")
    parser.add_argument("--agent-dir", type=Path, default=AGENT_DIR)
    parser.add_argument("--voice", default="Kyoko")
    parser.add_argument("--tts", choices=("say", "voicevox"),
                        default="say" if sys.platform == "darwin" else "voicevox")
    parser.add_argument("--voicevox-url", default="http://127.0.0.1:50021")
    parser.add_argument("--speaker", type=int, default=3, help="VOICEVOX style ID")
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--echo", action="store_true", help="STT/TTS test, without the agent")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    pipeline = Pipeline(args.whisper_model, whisper=args.whisper, codex=args.codex,
                        codex_model=args.codex_model, voice=args.voice, timeout=args.timeout,
                        echo=args.echo, tts=args.tts, voicevox_url=args.voicevox_url,
                        speaker=args.speaker, agent_dir=args.agent_dir, claude=args.claude)
    try:
        pipeline.check()
        server = ThreadingHTTPServer((args.host, args.port), Handler)
    except BaseException:
        pipeline.close()
        raise
    server.jobs = Jobs(pipeline)
    def stop(signum, frame):
        pipeline.close()
        threading.Thread(target=server.shutdown, daemon=True).start()
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    logging.info("Listening on http://%s:%s/talk (admin: http://%s:%s/admin)",
                 *(server.server_address * 2))
    try:
        server.serve_forever()
    finally:
        pipeline.close()
        server.server_close()
        if server.jobs.worker:
            server.jobs.worker.join(timeout=5)


if __name__ == "__main__":
    main()
