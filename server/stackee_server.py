#!/usr/bin/env python3
"""LAN voice receiver: WAV → whisper-cli → codex exec → say or VOICEVOX.

Python standard library only. POST /talk accepts WAV; poll the returned Location
for status, then GET its /audio endpoint for 16 kHz, signed little-endian PCM.
"""
from __future__ import annotations

import argparse
import array
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

RATE = 16000
MAX_SECONDS = 30
MAX_UPLOAD = RATE * 2 * MAX_SECONDS + 4096
MAX_REPLY_BYTES = RATE * 2 * MAX_SECONDS
PROMPT = (
    "あなたはキーボードの音声会話アシスタントです。日本語の話し言葉で、"
    "1〜2文、60文字以内で返答してください。見出し、箇条書き、URL、"
    "コード、絵文字は不要です。ツールの使用、ファイルの読み書き、"
    "コマンド実行はせず、会話にだけ答えてください。"
    "返答はそのまま音声で読み上げます。URL、ドメイン名、リンク、"
    "Markdownのリンク記法、出典リンク、引用マーカーを絶対に含めないでください。"
    "参照先の案内ではなく、質問への答えを普通の文章だけで伝えてください。"
)


def read_audio(data):
    try:
        with wave.open(io.BytesIO(data), "rb") as wav:
            if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(),
                    wav.getcomptype()) != (1, 2, RATE, "NONE"):
                raise ValueError("Expected uncompressed 16000 Hz / 16-bit / mono WAV")
            frames = wav.getnframes()
            if not int(RATE * .3) <= frames <= RATE * MAX_SECONDS:
                raise ValueError("Recording must be between 0.3 and 30 seconds")
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


class Pipeline:
    def __init__(self, model, whisper="whisper-cli", codex="codex", codex_model=None,
                 voice="Kyoko", timeout=120, echo=False, tts="say",
                 voicevox_url="http://127.0.0.1:50021", speaker=3):
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
        self.history = []
        self.lock = threading.Lock()
        self.process = None
        self.closed = False

    def check(self):
        for cmd in (self.whisper,) + (("say",) if self.tts == "say" else ()) + (() if self.echo else (self.codex,)):
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
        return limit_speaker_peak(read_audio(speech))

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

    def __call__(self, data):
        pcm = read_audio(data)
        if peak(pcm) < 150:
            return {"state": "ignored", "reason": "silence"}, b""
        with tempfile.TemporaryDirectory(prefix="stackee-talk-") as tmp:
            root = Path(tmp)
            wav = root / "input.wav"
            wav.write_bytes(data)
            text = clean_transcript(self.run(
                [self.whisper, "-m", self.model, "-f", str(wav), "-l", "ja", "-nt", "-np"], tmp))
            if not text:
                return {"state": "ignored", "reason": "no_speech"}, b""
            if self.echo:
                reply = text
            else:
                out = root / "reply.txt"
                argv = [self.codex, "exec", "--ephemeral", "--ignore-user-config",
                        "--skip-git-repo-check", "--sandbox", "read-only",
                        "--color", "never", "-o", str(out)]
                if self.codex_model:
                    argv += ["--model", self.codex_model]
                argv += ["-"]
                prompt = PROMPT + "\n\nこれまでの会話:\n" + json.dumps(
                    self.history[-4:], ensure_ascii=False) + "\nユーザー: " + text
                self.run(argv, tmp, input=prompt.encode("utf-8"))
                reply = out.read_text().strip() if out.exists() else ""
                if not reply:
                    raise RuntimeError("codex returned no final message")
            # Keep synthesis and device memory bounded even when the model ignores brevity.
            reply = clean_reply(reply)[:120]
            audio = self.synthesize(reply, root)
            if len(audio) > MAX_REPLY_BYTES:
                raise RuntimeError("Reply audio exceeds device limit")
            self.history.append({"user": text, "assistant": reply})
            del self.history[:-4]
            return {"state": "done", "transcript": text, "reply": reply}, audio


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
        try:
            result, audio = self.pipeline(data)
            result["audio"] = audio
        except Exception as exc:
            logging.exception("Voice job %s failed", ident)
            result = {"state": "error", "error": str(exc)}
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

    def do_GET(self):
        if self.path == "/health":
            return self.send(200, {"ok": True, "agent": "codex exec", "busy": self.server.jobs.busy})
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

    def do_POST(self):
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
    parser.add_argument("--voice", default="Kyoko")
    parser.add_argument("--tts", choices=("say", "voicevox"),
                        default="say" if sys.platform == "darwin" else "voicevox")
    parser.add_argument("--voicevox-url", default="http://127.0.0.1:50021")
    parser.add_argument("--speaker", type=int, default=3, help="VOICEVOX style ID")
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--echo", action="store_true", help="STT/TTS test, without Codex")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
    pipeline = Pipeline(args.whisper_model, args.whisper, args.codex,
                        args.codex_model, args.voice, args.timeout, args.echo,
                        args.tts, args.voicevox_url, args.speaker)
    pipeline.check()
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.jobs = Jobs(pipeline)
    def stop(signum, frame):
        pipeline.close()
        threading.Thread(target=server.shutdown, daemon=True).start()
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    logging.info("Listening on http://%s:%s/talk", *server.server_address)
    try:
        server.serve_forever()
    finally:
        pipeline.close()
        server.server_close()
        if server.jobs.worker:
            server.jobs.worker.join(timeout=5)


if __name__ == "__main__":
    main()
