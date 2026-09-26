#!/usr/bin/env python3
"""LAN voice receiver: WAV → whisper-cli → the configured agent → say or VOICEVOX.

Python standard library only. POST /talk accepts WAV; poll the returned Location
for status, then GET its /audio endpoint for 16 kHz, signed little-endian PCM. The
status carries the caption pages timed against that PCM, which /subtitles also serves.
POST /look accepts a camera JPEG instead: the agent looks at it in the same
conversation and its remark comes back through exactly the same job, audio and captions.
GET /admin serves the browser page that switches agent, model, effort and instructions.
POST /key {"key":"CSTM_n"} runs what /admin set for that key: a prompt to the agent (a job
like /look) or a command line here. `bin/stackee-say` (POST /say, local only) queues an
utterance in the inbox, which the device picks up with GET /inbox?after=&wait=&job=.
"""
from __future__ import annotations

import argparse
import array
from collections import deque
import io
import ipaddress
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
MAX_IMAGE = 512 * 1024
# What the agent is asked with every /look photo. The photo joins the same conversation,
# so a later spoken question can refer to it.
LOOK_PROMPT = ("ユーザーが stackee のカメラで今撮った写真です。"
               "何が写っているかを見て、短く話しかけてください。")
AGENT_DIR = Path(__file__).resolve().parent / "agent"
ADMIN_HTML = Path(__file__).resolve().parent / "admin.html"
# `stackee-say` lives here; a key's command finds it on its PATH.
BIN_DIR = Path(__file__).resolve().parent / "bin"

# The device's own keys CSTM_0..CSTM_9. What each does is set in keys.json from /admin.
CUSTOM_KEYS = tuple("CSTM_%d" % n for n in range(10))
KEY_MODES = ("none", "prompt", "command")
MAX_KEY_BODY = 256
KEY_TIMEOUT = 300
MAX_KEY_TIMEOUT = 600
# The tail of a key command's stdout / stderr that reaches the log.
KEY_OUTPUT_TAIL = 4096
# The inbox: utterances for the device to pick up, from `stackee-say` or anything local.
INBOX_SIZE = 16
INBOX_TTL = 300
MAX_INBOX_WAIT = 25
MAX_SAY_TEXT = 4096
MAX_SAY_BODY = 64 * 1024


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


def read_jpeg(data):
    """Accept one whole JPEG file: it opens with SOI (FFD8) and closes with EOI (FFD9)."""
    if len(data) < 4 or data[:2] != b"\xff\xd8" or data[-2:] != b"\xff\xd9":
        raise ValueError("Expected a JPEG image (FFD8 ... FFD9)")
    return data


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


FALLBACK_REPLY = "すみません。回答をうまくまとめられませんでした。"
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
        return FALLBACK_REPLY
    return text


SENTENCE_MARKS = "。．！？!?\n"
CLAUSE_MARKS = "、，,"
SILENCE = b"\0\0" * (RATE * 150 // 1000)   # a breath between sentences
BYTES_PER_MS = RATE * 2 // 1000

# Subtitles: one televised line is 15 full-width characters, and the device draws
# ASCII half as wide. A page never opens with punctuation or a closing bracket.
SUBTITLE_COLUMNS = 15
SUBTITLE_MAX_LINES = 48
SUBTITLE_MAX_BYTES = 4096
# The device fetches the status with one request; a second one for the subtitles cost it
# about eight seconds, so they travel inside the status JSON, which stays small enough
# for a fixed device buffer.
MAX_JOB_BYTES = 8192
# A caption-only utterance (`stackee-say --no-voice`) turns a page this often.
SUBTITLE_ONLY_PAGE_MS = 2500
NO_PAGE_START = "。、．，,.！？!?」』）)】〕》〉］]｝}・ー:;：；"


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


def page_width(text):
    """Display width in columns: ASCII is half a column wide, everything else one."""
    return sum(.5 if " " <= character <= "~" else 1. for character in text)


def opens_badly(text, index):
    """True when the page starting at `index` would open with punctuation."""
    rest = text[index:].lstrip()
    return bool(rest) and rest[0] in NO_PAGE_START


def split_columns(text, columns=SUBTITLE_COLUMNS):
    """Cut text into pieces of at most `columns` columns, none opening with punctuation.

    The pieces are as even as possible: what is left is always shared between the
    fewest pages that can hold it, so a long clause never ends in a stub page.
    """
    pieces, start = [], 0
    while start < len(text):
        rest = page_width(text[start:])
        target = rest / -(-rest // columns)   # ceil division: the fewest pages left
        end, width, best = start, 0., None
        while end < len(text) and width + page_width(text[end]) <= columns:
            width += page_width(text[end])
            end += 1
            if best is None or abs(width - target) < best[1]:
                best = end, abs(width - target)
        end = best[0]
        while start + 1 < end < len(text) and opens_badly(text, end):
            end -= 1
        pieces.append(text[start:end])
        start = end
    return pieces


def clause_spans(text):
    """Character ranges of the clauses of one sentence, in order, covering the text."""
    spans, start = [], 0
    for index, character in enumerate(text):
        if character in CLAUSE_MARKS + SENTENCE_MARKS:
            spans.append((start, index + 1))
            start = index + 1
    if start < len(text):
        spans.append((start, len(text)))
    return spans


def subtitle_pages(text):
    """Pages of one sentence as (character offset within the sentence, page text)."""
    pages = []
    for start, end in clause_spans(text):
        offset = start
        for piece in split_columns(text[start:end]):
            page = " ".join(piece.split())
            if page:
                pages.append((offset, page))
            offset += len(piece)
    return pages


def subtitle_body(sentences, max_lines=SUBTITLE_MAX_LINES, max_bytes=SUBTITLE_MAX_BYTES):
    """`<start_ms>\\t<text>` lines for [(start_ms, duration_ms, sentence)].

    A sentence boundary is exact, because its start comes from the PCM already joined.
    Inside a sentence the pages share its duration in proportion to their characters.
    """
    lines = []
    for start_ms, duration_ms, text in sentences:
        for offset, page in subtitle_pages(text):
            lines.append((round(start_ms + duration_ms * offset / len(text)), page))
    return subtitle_lines(lines, max_lines, max_bytes)


def subtitle_lines(lines, max_lines=SUBTITLE_MAX_LINES, max_bytes=SUBTITLE_MAX_BYTES):
    """The subtitle body for [(start_ms, page)], cut at the line and byte limits."""
    body = b""
    for start, page in lines[:max_lines]:
        line = (str(start) + "\t" + page + "\n").encode("utf-8")
        if len(body) + len(line) > max_bytes:
            break
        body += line
    return body


def subtitle_only_body(text, page_ms=None):
    """Captions for a text that is shown but not spoken: the same pages, evenly spaced."""
    page_ms = SUBTITLE_ONLY_PAGE_MS if page_ms is None else page_ms
    pages = [page for sentence in split_parts(text, SENTENCE_MARKS)
             for _, page in subtitle_pages(sentence)]
    return subtitle_lines([(index * page_ms, page) for index, page in enumerate(pages)])


def job_body(item, subtitles, limit=MAX_JOB_BYTES):
    """The status JSON with the subtitles inline, and the subtitles that survived.

    The device reads the status into a fixed buffer, so end pages are dropped until the
    JSON fits. /subtitles then serves exactly what the status carried.
    """
    lines = subtitles.splitlines(keepends=True)
    while True:
        result = dict(item)
        if lines:
            result["subtitles"] = b"".join(lines).decode("utf-8")
        else:
            result.pop("subtitles_url", None)
        body = json.dumps(result, ensure_ascii=False).encode("utf-8")
        if len(body) <= limit or not lines:
            return body, b"".join(lines)
        lines.pop()


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
        # /say synthesises outside the job slot, so more than one child may run at once.
        self.processes = set()
        # Every synthesis (a job's reply or /say) takes turns: the engine shares the GPU
        # with whisper, and fit() keeps its last failure on the instance.
        self.synth_lock = threading.Lock()
        self.closed = False

    @property
    def process(self):
        """One running child, or None (kept for callers that expect a single slot)."""
        with self.lock:
            return next(iter(self.processes), None)

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

        Returns the text actually spoken, its PCM, and the subtitle body, whose page
        times come from where each sentence starts in that PCM.
        """
        with self.synth_lock:
            return self._fit(reply, root, limit)

    def _fit(self, reply, root, limit):
        self.failure = None
        chunks, kept, used, spoken = [], "", 0, []
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
            spoken.append(((used + len(gap)) / BYTES_PER_MS, len(audio) / BYTES_PER_MS, unit))
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
        return kept, limit_speaker_peak(b"".join(chunks)), subtitle_body(spoken)

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
            self.processes.add(proc)
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
                self.processes.discard(proc)

    @staticmethod
    def kill(proc):
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass

    def close(self):
        with self.lock:
            self.closed = True
            for proc in self.processes:
                self.kill(proc)
        if self.agent:
            self.agent.close()

    def answer(self, question, root, timings, started, image=None, transcript=None):
        """The agent's reply to one input, cleaned, fitted and spoken, as the job result."""
        stage = time.monotonic()
        if self.echo:
            reply = question if image is None else (
                "写真を受け取りました。" + str(len(image) // 1024) + "キロバイトです。")
        else:
            reply = (self.agent.ask(question) if image is None
                     else self.agent.ask(question, image))
            if not reply:
                raise RuntimeError("codex returned no final message")
        reply = clean_reply(reply)
        timings["codex_ms"] = round((time.monotonic() - stage) * 1000, 2)
        stage = time.monotonic()
        # Keep synthesis and device memory bounded even when the model ignores brevity.
        reply, audio, subtitles = self.fit(reply, root)
        timings["tts_ms"] = round((time.monotonic() - stage) * 1000, 2)
        timings["total_ms"] = round((time.monotonic() - started) * 1000, 2)
        if transcript is None:
            transcript = "" if image is not None else question
        return {"state": "done", "transcript": transcript,
                "reply": reply, "timings": timings, "subtitles": subtitles}, audio

    def look(self, data):
        """A camera photo instead of speech: no transcription, the same reply path."""
        started = time.monotonic()
        image = read_jpeg(data)
        with tempfile.TemporaryDirectory(prefix="stackee-look-") as tmp:
            timings = {"prepare_ms": round((time.monotonic() - started) * 1000, 2)}
            return self.answer(LOOK_PROMPT, Path(tmp), timings, started, image)

    def prompt(self, text):
        """A CSTM key's saved prompt: like /look without the photo, the transcript empty."""
        started = time.monotonic()
        with tempfile.TemporaryDirectory(prefix="stackee-key-") as tmp:
            timings = {"prepare_ms": round((time.monotonic() - started) * 1000, 2)}
            return self.answer(text, Path(tmp), timings, started, transcript="")

    def say(self, text, voice=True):
        """What `stackee-say` asks for: (reply, PCM, subtitles) ready for the inbox.

        The text goes through the same clean-up as an agent reply. With a voice it is
        fitted and spoken like one; without, the captions are paged by the same rules
        and shown SUBTITLE_ONLY_PAGE_MS apart from 0.
        """
        reply = clean_reply(text)
        if reply == FALLBACK_REPLY and FALLBACK_REPLY not in text:
            raise ValueError("nothing_to_say")
        if not voice:
            return reply, b"", subtitle_only_body(reply)
        with tempfile.TemporaryDirectory(prefix="stackee-say-") as tmp:
            return self.fit(reply, Path(tmp))

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
            return self.answer(text, root, timings, started)


def default_keys():
    return {key: {"mode": "none", "prompt": "", "command": "", "timeout": KEY_TIMEOUT}
            for key in CUSTOM_KEYS}


def normalize_keys(raw):
    """Validate keys.json: every CSTM key, its mode, its text and its time limit.

    Missing keys are unset. The prompt and the command are both kept whatever the mode,
    so switching a key back does not lose what was typed.
    """
    if not isinstance(raw, dict):
        raise ValueError("キー設定はオブジェクトで指定してください")
    unknown = sorted(set(raw) - set(CUSTOM_KEYS))
    if unknown:
        raise ValueError("知らないキーです: " + unknown[0])
    result = default_keys()
    for key in CUSTOM_KEYS:
        entry = raw.get(key)
        if entry is None:
            continue
        if not isinstance(entry, dict):
            raise ValueError(key + ": 設定はオブジェクトで指定してください")
        mode = entry.get("mode", "none")
        if mode not in KEY_MODES:
            raise ValueError(key + ": 方式は none / prompt / command のどれかです")
        texts = {}
        for name in ("prompt", "command"):
            value = entry.get(name, "")
            if value is None:
                value = ""
            if not isinstance(value, str) or "\0" in value:
                raise ValueError(key + ": " + name + " は文字列で指定してください")
            texts[name] = value.replace("\r\n", "\n")
        timeout = entry.get("timeout", KEY_TIMEOUT)
        if (isinstance(timeout, bool) or not isinstance(timeout, (int, float))
                or timeout != int(timeout) or not 1 <= timeout <= MAX_KEY_TIMEOUT):
            raise ValueError(key + ": 時間切れは 1〜%d 秒の整数です" % MAX_KEY_TIMEOUT)
        if mode == "prompt" and not texts["prompt"].strip():
            raise ValueError(key + ": プロンプトが空です")
        if mode == "command" and not texts["command"].strip():
            raise ValueError(key + ": コマンドが空です")
        result[key] = {"mode": mode, "prompt": texts["prompt"], "command": texts["command"],
                       "timeout": int(timeout)}
    return result


class KeySettings:
    """server/agent/keys.json, read afresh on every press so a save applies at once.

    Like agent.json it is untracked and restored from defaults/keys.json when missing.
    """
    def __init__(self, directory):
        self.directory = Path(directory).resolve()
        self.path = self.directory / "keys.json"
        source = self.directory / "defaults" / "keys.json"
        if not self.path.exists() and source.exists():
            shutil.copyfile(source, self.path)

    def read(self):
        if not self.path.exists():
            return default_keys()
        return normalize_keys(json.loads(self.path.read_text(encoding="utf-8")))

    def write(self, raw):
        keys = normalize_keys(raw)
        stackee_agent.write_atomic(self.path, json.dumps(keys, ensure_ascii=False, indent=2) + "\n")
        return keys


class Inbox:
    """Utterances waiting for the device, oldest first: at most 16, each for five minutes.

    seq counts up from 1 for the life of the server. A reader remembers the last seq it
    played and asks for the next one, so nothing that arrived before it looked is replayed.
    """
    def __init__(self, size=INBOX_SIZE, ttl=INBOX_TTL):
        self.size = size
        self.ttl = ttl
        self.cond = threading.Condition()
        self.items = deque()
        self.seq = 0

    def prune(self):
        now = time.monotonic()
        while self.items and now - self.items[0]["created"] > self.ttl:
            self.items.popleft()

    def put(self, reply, audio, subtitles):
        with self.cond:
            self.seq += 1
            self.items.append({"seq": self.seq, "created": time.monotonic(), "reply": reply,
                               "audio": audio, "subtitles": subtitles})
            while len(self.items) > self.size:
                self.items.popleft()
            self.prune()
            self.cond.notify_all()
            return self.seq

    def poke(self):
        """Wake the long polls, e.g. when the job one of them follows has finished."""
        with self.cond:
            self.cond.notify_all()

    def after(self, seq):
        """The oldest utterance newer than seq (call with cond held)."""
        self.prune()
        return next((dict(item) for item in self.items if item["seq"] > seq), None)

    def audio(self, seq):
        with self.cond:
            self.prune()
            item = next((item for item in self.items if item["seq"] == seq), None)
            return item["audio"] if item else None

    def summary(self):
        with self.cond:
            self.prune()
            return {"count": len(self.items), "seq": self.seq}


def inbox_body(item, extra=None):
    """One utterance as the device reads it: the /talk done fields, audio under /inbox."""
    result = {"state": "say", "seq": item["seq"], "reply": item["reply"]}
    if item["audio"]:
        result.update(audio_url="/inbox/%d/audio" % item["seq"],
                      audio_bytes=len(item["audio"]))
    result.update(sample_rate=RATE, channels=1, sample_width=2)
    result.update(extra or {})
    return job_body(result, item["subtitles"])[0]


class KeyCommandError(RuntimeError):
    """A key's command failed or ran out of time; the message is what the device shows."""


class Jobs:
    """One conversation at a time; results expire after five minutes, max 8 retained.

    /talk, /look and a CSTM key's prompt or command share the single slot: while any
    runs, all of them answer 409.
    """
    def __init__(self, pipeline, keys=None, inbox=None, say_url=None):
        self.pipeline = pipeline
        self.lock = threading.Lock()
        self.entries = {}
        self.busy = False
        self.worker = None
        self.keys = keys
        self.inbox = inbox if inbox is not None else Inbox()
        self.say_url = say_url
        self.command = None
        self.closed = False
        # The latest CSTM presses for the admin page: key, mode, result, time.
        self.presses = deque(maxlen=10)

    def press(self, key, mode, result, ident=None):
        with self.lock:
            self.presses.appendleft({"key": key, "mode": mode, "result": result, "job": ident,
                                     "at": time.strftime("%Y-%m-%d %H:%M:%S")})

    def recent_presses(self):
        with self.lock:
            return [dict(entry) for entry in self.presses]

    def run_command(self, key, command, timeout):
        """A key's command line under /bin/sh in the agent directory; the slot is held till it exits.

        Output goes to temporary files rather than pipes, so a child the command leaves
        running in the background does not keep the job open. Only the command itself is
        waited for; on a timeout its whole process group is killed.
        """
        env = dict(os.environ)
        env["STACKEE_KEY"] = key
        if self.say_url:
            env["STACKEE_SAY_URL"] = self.say_url
        env["PATH"] = str(BIN_DIR) + os.pathsep + env.get("PATH", os.defpath)
        started = time.monotonic()
        with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
            with self.lock:
                if self.closed:
                    raise KeyCommandError(key + " 中止 (サーバー停止中)")
                try:
                    proc = subprocess.Popen(["/bin/sh", "-c", command], cwd=str(self.keys.directory),
                                            env=env, stdin=subprocess.DEVNULL, stdout=out,
                                            stderr=err, start_new_session=True)
                except OSError as exc:
                    logging.error("key-command %s could not start: %s", key, exc)
                    raise KeyCommandError(key + " 起動失敗") from exc
                self.command = proc
            try:
                try:
                    code = proc.wait(timeout=timeout)
                    timed_out = False
                except subprocess.TimeoutExpired:
                    Pipeline.kill(proc)
                    code = proc.wait()
                    timed_out = True
            finally:
                with self.lock:
                    self.command = None
            outputs = []
            for stream in (out, err):
                stream.seek(0, os.SEEK_END)
                size = stream.tell()
                stream.seek(max(0, size - KEY_OUTPUT_TAIL))
                text = stream.read().decode("utf-8", "replace")
                outputs.append(text if size <= KEY_OUTPUT_TAIL else "…" + text)
        logging.info("key-command %s", json.dumps({
            "key": key, "exit_code": code, "timed_out": timed_out,
            "ms": round((time.monotonic() - started) * 1000, 2),
            "stdout": outputs[0], "stderr": outputs[1]}, ensure_ascii=False))
        if timed_out:
            raise KeyCommandError(key + " 時間切れ")
        if code < 0:
            raise KeyCommandError("%s 失敗 (シグナル %d)" % (key, -code))
        if code:
            raise KeyCommandError("%s 失敗 (終了コード %d)" % (key, code))
        return {"state": "done", "reply": "", "exit_code": 0, "silent": True}, b""

    def close(self):
        with self.lock:
            self.closed = True
            if self.command:
                Pipeline.kill(self.command)

    def prune(self):
        for key in list(self.entries):
            if (self.entries[key]["state"] != "processing"
                    and time.monotonic() - self.entries[key]["created"] > 300):
                del self.entries[key]

    def submit(self, data, kind="talk"):
        with self.lock:
            if self.busy:
                return None
            self.prune()
            while len(self.entries) >= 8:
                del self.entries[next(iter(self.entries))]
            ident = uuid.uuid4().hex
            self.entries[ident] = {"created": time.monotonic(), "state": "processing"}
            self.busy = True
            self.worker = threading.Thread(target=self.process, args=(ident, data, kind),
                                           daemon=True)
            self.worker.start()
            return ident

    def process(self, ident, data, kind="talk"):
        started = time.monotonic()
        try:
            if kind == "key" and data["mode"] == "command":
                result, audio = self.run_command(data["key"], data["command"], data["timeout"])
            elif kind == "key":
                result, audio = self.pipeline.prompt(data["prompt"])
            else:
                work = self.pipeline if kind == "talk" else self.pipeline.look
                result, audio = work(data)
            result["audio"] = audio
        except KeyCommandError as exc:
            # The key-command line already has the exit code and the output.
            logging.error("Key job %s failed: %s", ident, exc)
            result = {"state": "error", "error": str(exc)}
        except Exception as exc:
            logging.exception("Voice job %s failed", ident)
            result = {"state": "error", "error": str(exc)}
        if kind == "key":
            kind = "key:" + data["key"] + ":" + data["mode"]
        logging.info("job-timing %s", json.dumps({"job": ident, "kind": kind,
                     "state": result["state"],
                     "worker_ms": round((time.monotonic() - started) * 1000, 2),
                     "timings": result.get("timings", {})}))
        with self.lock:
            if ident in self.entries:
                self.entries[ident].update(result)
                self.entries[ident]["created"] = time.monotonic()
            for entry in self.presses:
                if entry["job"] == ident:
                    entry["result"] = result["state"]
                    if result.get("error"):
                        entry["error"] = result["error"]
            self.busy = False
        # A long poll following this job (GET /inbox?job=) answers now.
        self.inbox.poke()

    def get(self, ident):
        with self.lock:
            self.prune()
            item = self.entries.get(ident)
            return dict(item) if item else None


def local_address(peer, own):
    """True when a connection from `peer` to our socket at `own` comes from this machine.

    Loopback always does. So does a connection whose source is our own address: that is
    how a local process reaches a server listening on one LAN address only.
    """
    try:
        peer = ipaddress.ip_address(peer.split("%")[0])
        own = ipaddress.ip_address(own.split("%")[0])
    except ValueError:
        return False
    peer = getattr(peer, "ipv4_mapped", None) or peer
    own = getattr(own, "ipv4_mapped", None) or own
    return peer.is_loopback or peer == own


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
        jobs = self.server.jobs
        keys, keys_error = None, None
        if jobs.keys is not None:
            try:
                keys = jobs.keys.read()
            except (OSError, ValueError) as exc:
                keys_error = str(exc)
        return self.send(200, {"config": config, "instructions": instructions,
                               "status": agent.status(), "busy": jobs.busy,
                               "choices": {"codex_effort": list(stackee_agent.CODEX_EFFORTS),
                                           "claude_effort": list(stackee_agent.CLAUDE_EFFORTS),
                                           "key_modes": list(KEY_MODES),
                                           "key_timeout_max": MAX_KEY_TIMEOUT},
                               "keys": keys, "keys_error": keys_error,
                               "key_presses": jobs.recent_presses(),
                               "inbox": jobs.inbox.summary()})

    def admin_keys(self):
        """PUT /admin/api/keys: saved at once, used from the next press."""
        keys = self.server.jobs.keys
        if keys is None:
            return self.send(503, {"error": "keys_disabled"})
        if not self.admin_allowed():
            return None
        data = self.admin_body()
        if data is None:
            return None
        try:
            saved = keys.write(json.loads(data.decode("utf-8")))
        except (ValueError, UnicodeDecodeError) as exc:
            return self.send(400, {"error": str(exc)})
        except OSError as exc:
            return self.send(500, {"error": str(exc)})
        return self.send(200, {"saved": True, "keys": saved})

    def inbox_get(self, query):
        """GET /inbox[?after=&wait=&job=]: the next utterance, held open up to `wait` seconds."""
        jobs = self.server.jobs
        inbox = jobs.inbox
        if not query:
            return self.send(200, {"state": "empty", "seq": inbox.summary()["seq"]})
        params = {}
        for part in query.split("&"):
            name, equals, value = part.partition("=")
            if not equals or name not in ("after", "wait", "job") or name in params:
                return self.send(400, {"error": "bad_query"})
            params[name] = value
        after, wait, job = params.get("after"), params.get("wait", "0"), params.get("job")
        if (after is None or not re.fullmatch(r"[0-9]{1,15}", after)
                or not re.fullmatch(r"[0-9]{1,3}", wait)
                or (job is not None and not re.fullmatch(r"[0-9a-f]{32}", job))):
            return self.send(400, {"error": "bad_query"})
        after, wait = int(after), min(int(wait), MAX_INBOX_WAIT)
        deadline = time.monotonic() + wait
        with inbox.cond:
            while True:
                item = inbox.after(after)
                extra = {}
                if job is not None:
                    entry = jobs.get(job)
                    if entry is None:
                        extra = {"job_state": "not_found"}
                    elif entry["state"] != "processing":
                        extra = {"job_state": entry["state"]}
                        if entry.get("error"):
                            extra["error"] = entry["error"]
                remaining = deadline - time.monotonic()
                if item or extra or remaining <= 0:
                    latest = inbox.seq
                    break
                inbox.cond.wait(remaining)
        if item:
            return self.send(200, inbox_body(item, extra))
        return self.send(200, dict({"state": "empty", "seq": latest}, **extra))

    def local_client(self):
        """True for a request from this machine: loopback, or our own address talking to itself.

        The server may listen on one LAN address only (STACKEE_BIND_HOST); then a local
        `stackee-say` reaches it from that same address, which no other machine can use.
        """
        return local_address(self.client_address[0], self.connection.getsockname()[0])

    def say_post(self):
        """POST /say from this machine only: speak or caption a text via the inbox."""
        if not self.local_client():
            return self.send(403, {"error": "local_only"})
        if self.headers.get("Origin"):
            return self.send(403, {"error": "device_api_only"})
        if self.headers.get("Transfer-Encoding"):
            return self.send(400, {"error": "content_length_required"})
        if self.headers.get_content_type() != "application/json":
            return self.send(415, {"error": "expected_application_json"})
        lengths = self.headers.get_all("Content-Length", [])
        if len(lengths) != 1 or not lengths[0].isascii() or not lengths[0].isdigit():
            return self.send(411, {"error": "content_length_required"})
        length = int(lengths[0])
        if length > MAX_SAY_BODY:
            return self.send(413, {"error": "text_too_large"})
        try:
            request = json.loads(self.rfile.read(length).decode("utf-8"))
        except TimeoutError:
            return self.send(408, {"error": "upload_timeout"})
        except (ValueError, UnicodeDecodeError):
            return self.send(400, {"error": "invalid_json"})
        text = request.get("text") if isinstance(request, dict) else None
        voice = request.get("voice", True) if isinstance(request, dict) else None
        if not isinstance(text, str) or not isinstance(voice, bool):
            return self.send(400, {"error": "expected_text_and_voice"})
        if len(text.encode("utf-8")) > MAX_SAY_TEXT:
            return self.send(413, {"error": "text_too_large"})
        if not text.strip():
            return self.send(400, {"error": "text_empty"})
        started = time.monotonic()
        try:
            reply, audio, subtitles = self.server.jobs.pipeline.say(text, voice)
        except ValueError as exc:
            if str(exc) == "nothing_to_say":
                return self.send(400, {"error": "nothing_to_say"})
            logging.exception("say failed")
            return self.send(500, {"error": str(exc) or exc.__class__.__name__})
        except Exception as exc:
            logging.exception("say failed")
            return self.send(500, {"error": str(exc) or exc.__class__.__name__})
        seq = self.server.jobs.inbox.put(reply, audio, subtitles)
        logging.info("inbox-put %s", json.dumps({
            "seq": seq, "voice": voice, "chars": len(reply), "audio_bytes": len(audio),
            "ms": round((time.monotonic() - started) * 1000, 2)}))
        return self.send(200, {"seq": seq})

    def key_post(self):
        """POST /key {"key":"CSTM_n"}: run what the admin page set for that key."""
        if self.headers.get("Origin"):
            return self.send(403, {"error": "device_api_only"})
        if self.headers.get("Transfer-Encoding"):
            return self.send(400, {"error": "content_length_required"})
        if self.headers.get_content_type() != "application/json":
            return self.send(415, {"error": "expected_application_json"})
        lengths = self.headers.get_all("Content-Length", [])
        if len(lengths) != 1 or not lengths[0].isascii() or not lengths[0].isdigit():
            return self.send(411, {"error": "content_length_required"})
        length = int(lengths[0])
        if not 1 <= length <= MAX_KEY_BODY:
            return self.send(413, {"error": "key_body_too_large_or_empty"})
        try:
            request = json.loads(self.rfile.read(length).decode("utf-8"))
        except TimeoutError:
            return self.send(408, {"error": "upload_timeout"})
        except (ValueError, UnicodeDecodeError):
            return self.send(400, {"error": "invalid_json"})
        key = request.get("key") if isinstance(request, dict) else None
        if key not in CUSTOM_KEYS:
            return self.send(400, {"error": "unknown_key"})
        jobs = self.server.jobs
        if jobs.keys is None:
            return self.send(503, {"error": "keys_disabled"})
        try:
            setting = jobs.keys.read()[key]
        except (OSError, ValueError) as exc:
            logging.error("key-press %s: keys.json unreadable: %s", key, exc)
            return self.send(500, {"error": "keys_unreadable"})
        mode = setting["mode"]
        if mode == "none":
            logging.info("key-press %s", json.dumps({"key": key, "mode": mode, "result": "ignored"}))
            jobs.press(key, mode, "ignored")
            return self.send(200, {"state": "ignored", "key": key})
        ident = jobs.submit(dict(setting, key=key), "key")
        if ident is None:
            logging.info("key-press %s", json.dumps({"key": key, "mode": mode, "result": "busy"}))
            jobs.press(key, mode, "busy")
            return self.send(409, {"error": "busy"})
        logging.info("key-press %s", json.dumps({"key": key, "mode": mode, "job": ident}))
        jobs.press(key, mode, "processing", ident)
        location = "/jobs/" + ident
        return self.send(202, {"id": ident, "status_url": location, "mode": mode},
                         location=location)

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
        path, _, query = self.path.partition("?")
        if path == "/inbox":
            return self.inbox_get(query)
        spoken = re.fullmatch(r"/inbox/([0-9]{1,15})/audio", self.path)
        if spoken:
            audio = self.server.jobs.inbox.audio(int(spoken[1]))
            if not audio:
                return self.send(404, {"error": "not_found"})
            return self.send(200, audio, "application/octet-stream")
        match = re.fullmatch(r"/jobs/([0-9a-f]{32})(/audio|/subtitles)?", self.path)
        item = self.server.jobs.get(match[1]) if match else None
        if not item:
            return self.send(404, {"error": "not_found"})
        audio = item.pop("audio", b"")
        subtitles = item.pop("subtitles", b"")
        item.pop("created")
        # A key's command speaks, if at all, through the inbox: its job has no audio.
        silent = item.pop("silent", False)
        if match[2] == "/audio":
            if item["state"] != "done":
                return self.send(409, {"error": "audio_not_ready"})
            if silent:
                return self.send(404, {"error": "not_found"})
            return self.send(200, audio, "application/octet-stream")
        body = None
        if item["state"] == "done" and not silent:
            job = "/jobs/" + match[1]
            item.update(audio_url=job + "/audio", sample_rate=RATE,
                        channels=1, sample_width=2, audio_bytes=len(audio))
            if subtitles:
                item["subtitles_url"] = job + "/subtitles"
            body, subtitles = job_body(item, subtitles)
        if match[2] == "/subtitles":
            if item["state"] == "processing":
                return self.send(409, {"error": "subtitles_not_ready"})
            if not subtitles:
                return self.send(404, {"error": "not_found"})
            return self.send(200, subtitles, "text/plain; charset=utf-8")
        return self.send(200, item if body is None else body)

    def do_PUT(self):
        if self.path == "/admin/api/keys":
            return self.admin_keys()
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
        if self.path == "/key":
            return self.key_post()
        if self.path == "/say":
            return self.say_post()
        # Per device route: content types, size range, the two error codes, the validator.
        route = {"/talk": (("audio/wav", "audio/x-wav"), 44, MAX_UPLOAD,
                           "recording_too_large_or_empty", "expected_audio_wav", read_audio),
                 "/look": (("image/jpeg",), 4, MAX_IMAGE,
                           "image_too_large_or_empty", "expected_image_jpeg", read_jpeg),
                 }.get(self.path)
        if route is None:
            return self.send(404, {"error": "not_found"})
        types, smallest, largest, too_large, wrong_type, validate = route
        # Device API only. No CORS; reject browser-originated posts explicitly.
        if self.headers.get("Origin"):
            return self.send(403, {"error": "device_api_only"})
        if self.headers.get("Transfer-Encoding"):
            return self.send(400, {"error": "content_length_required"})
        if self.headers.get_content_type() not in types:
            return self.send(415, {"error": wrong_type})
        lengths = self.headers.get_all("Content-Length", [])
        if len(lengths) != 1 or not lengths[0].isascii() or not lengths[0].isdigit():
            return self.send(411, {"error": "content_length_required"})
        length = int(lengths[0])
        if not smallest <= length <= largest:
            return self.send(413, {"error": too_large})
        try:
            data = self.rfile.read(length)
            if len(data) != length:
                raise ValueError("Incomplete request body")
            validate(data)
        except TimeoutError:
            return self.send(408, {"error": "upload_timeout"})
        except ValueError as exc:
            return self.send(400, {"error": str(exc)})
        ident = self.server.jobs.submit(data, self.path[1:])
        if ident is None:
            return self.send(409, {"error": "busy"})
        location = "/jobs/" + ident
        self.send(202, {"id": ident, "status_url": location}, location=location)


def say_url(address):
    """Where a key's command posts `stackee-say`: this server, reached from this machine.

    A wildcard listener is reached on loopback; a server bound to one LAN address only
    (STACKEE_BIND_HOST) must be reached on that address.
    """
    host, port = address[:2]
    if host in ("", "0.0.0.0"):
        host = "127.0.0.1"
    elif host == "::":
        host = "::1"
    if ":" in host:
        host = "[" + host + "]"
    return "http://%s:%d/say" % (host, port)


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
    server.jobs = Jobs(pipeline, keys=KeySettings(args.agent_dir),
                       say_url=say_url(server.server_address))
    def stop(signum, frame):
        pipeline.close()
        server.jobs.close()
        threading.Thread(target=server.shutdown, daemon=True).start()
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    logging.info("Listening on http://%s:%s/talk and /look (admin: http://%s:%s/admin)",
                 *(server.server_address * 2))
    logging.info("Key commands reach stackee-say at %s", server.jobs.say_url)
    try:
        server.serve_forever()
    finally:
        pipeline.close()
        server.jobs.close()
        server.server_close()
        if server.jobs.worker:
            server.jobs.worker.join(timeout=5)


if __name__ == "__main__":
    main()
