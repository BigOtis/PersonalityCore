#!/usr/bin/env python3
"""
Kokoro ONNX TTS worker for the LocalTalker UE5 plugin.

Uses the same JSON-over-pipes protocol as qwen_tts_worker.py so the C++ side
can treat both workers identically.

REQUEST  (one JSON line → stdin):
  {"cmd":"synthesize","text":"...","speaker":"af_bella","request_id":"...","streaming":true}
  {"cmd":"health"}
  {"cmd":"shutdown"}

RESPONSE (streaming):
  {"streaming_chunk":true,"request_id":"...","chunk_index":0,"pcm_base64":"...","sample_rate":24000,"num_samples":N,"num_channels":1}
  ...
  {"ok":true,"streaming_done":true,"request_id":"...","total_samples":N,"num_channels":1,"total_chunks":N,"metrics":{...}}

RESPONSE (non-streaming):
  {"ok":true,"sample_rate":24000,"num_samples":N,"num_channels":1,"pcm_base64":"...","metrics":{...}}

RESPONSE (health):
  {"ok":true,"ready":true,"backend":"kokoro-onnx","speakers":[...],"pid":N,...}

Model files (~330 MB ONNX + ~47 MB voices) are downloaded automatically on first
run from the kokoro-onnx GitHub releases. Subsequent runs use the local cache.
Requires: kokoro-onnx, numpy  (no PyTorch, no CUDA needed)
"""

import sys
import os
import json
import base64
import time
import argparse
import re
import traceback

import numpy as np

# ---------------------------------------------------------------------------
# Sentence splitter for streaming chunks
# ---------------------------------------------------------------------------

_SENT_RE = re.compile(r'(?<=[.!?])\s+')

def _split_sentences(text: str):
    """Split text into sentences for per-sentence streaming chunks."""
    parts = _SENT_RE.split(text.strip())
    out = [p.strip() for p in parts if p.strip()]
    return out if out else [text.strip()]


# ---------------------------------------------------------------------------
# Logging / wire helpers
# ---------------------------------------------------------------------------

def _log(msg: str):
    print(msg, file=sys.stderr, flush=True)

def _write(obj: dict):
    print(json.dumps(obj, ensure_ascii=False), flush=True)


# ---------------------------------------------------------------------------
# Model file resolution and auto-download
# ---------------------------------------------------------------------------

_MODEL_FILENAME  = "kokoro-v1.0.onnx"
_VOICES_FILENAME = "voices-v1.0.bin"
_RELEASES_BASE   = "https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0"

def _resolve_cache_dir(cache_dir_arg: str) -> str:
    if cache_dir_arg:
        return cache_dir_arg
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
        return os.path.join(base, "LocalTalker", "Kokoro")
    return os.path.join(os.path.expanduser("~"), ".cache", "localtalker", "kokoro")


def _download_file(url: str, dest: str):
    import urllib.request
    tmp = dest + ".tmp"
    _log(f"Downloading {os.path.basename(dest)} ...")
    try:
        def _hook(count, block, total):
            if total > 0 and count % 200 == 0:
                pct = min(100, count * block * 100 // total)
                _log(f"  {os.path.basename(dest)}: {pct}%")
        urllib.request.urlretrieve(url, tmp, reporthook=_hook)
        os.replace(tmp, dest)
        _log(f"Downloaded {os.path.basename(dest)} ({os.path.getsize(dest):,} bytes)")
    except Exception as exc:
        if os.path.exists(tmp):
            try:
                os.remove(tmp)
            except OSError:
                pass
        raise RuntimeError(f"Download failed for {url}: {exc}") from exc


def _resolve_model_files(cache_dir: str):
    """Return (model_path, voices_path), downloading if needed."""
    os.makedirs(cache_dir, exist_ok=True)
    model_path  = os.path.join(cache_dir, _MODEL_FILENAME)
    voices_path = os.path.join(cache_dir, _VOICES_FILENAME)

    for fname, fpath in [(_MODEL_FILENAME, model_path), (_VOICES_FILENAME, voices_path)]:
        if os.path.isfile(fpath) and os.path.getsize(fpath) > 0:
            _log(f"Found cached {fname} at {fpath}")
            continue
        _download_file(f"{_RELEASES_BASE}/{fname}", fpath)

    return model_path, voices_path


# ---------------------------------------------------------------------------
# Known voices (fallback list if kokoro can't enumerate them)
# ---------------------------------------------------------------------------

_KNOWN_VOICES = [
    "af_alloy", "af_aoede", "af_bella", "af_heart", "af_jessica", "af_kore",
    "af_nicole", "af_nova", "af_river", "af_sarah", "af_sky",
    "am_adam", "am_echo", "am_eric", "am_fenrir", "am_liam",
    "am_michael", "am_onyx", "am_puck",
    "bf_emma", "bf_isabella", "bm_george", "bm_lewis",
]


# ---------------------------------------------------------------------------
# KokoroWorker
# ---------------------------------------------------------------------------

class KokoroWorker:
    def __init__(self, model_path: str, voices_path: str, speed: float = 1.0):
        _log(f"Loading Kokoro ONNX model: {model_path}")
        from kokoro_onnx import Kokoro  # type: ignore
        self.kokoro = Kokoro(model_path, voices_path)
        self.speed = speed
        _log(f"Kokoro model loaded. voices={len(self.get_voices())}")

    def get_voices(self):
        try:
            return sorted(self.kokoro.get_voices())
        except Exception:
            return list(_KNOWN_VOICES)

    def synthesize_sentence(self, text: str, voice: str, speed: float | None = None):
        """Return (pcm_bytes, sample_rate). pcm_bytes is signed 16-bit PCM, mono."""
        s = speed if speed is not None else self.speed
        samples, sr = self.kokoro.create(text, voice=voice, speed=s, lang="en-us")
        pcm = (np.clip(samples, -1.0, 1.0) * 32767.0).astype(np.int16).tobytes()
        return pcm, int(sr)


# ---------------------------------------------------------------------------
# Synthesize handler
# ---------------------------------------------------------------------------

def _handle_synthesize(worker: KokoroWorker, req: dict):
    text    = (req.get("text") or "").strip()
    voice   = req.get("speaker") or "af_bella"
    req_id  = req.get("request_id") or f"kokoro-{int(time.time()*1000)}"
    speed   = req.get("speed", None)
    streaming = bool(req.get("streaming", False))

    if not text:
        _write({"ok": False, "error": "Empty text"})
        return

    # Validate / fallback voice
    available = worker.get_voices()
    if voice not in available:
        fallback = "af_bella" if "af_bella" in available else (available[0] if available else "af_bella")
        _log(f"Voice '{voice}' not in available list, falling back to '{fallback}'.")
        voice = fallback

    _log(f"synthesize.begin req_id={req_id} chars={len(text)} voice='{voice}' streaming={streaming}")
    t0 = time.time()

    if streaming:
        sentences = _split_sentences(text)
        total_samples = 0
        total_chunks = 0
        for i, sent in enumerate(sentences):
            if not sent:
                continue
            try:
                pcm, sr = worker.synthesize_sentence(sent, voice, speed)
            except Exception as exc:
                _log(f"streaming chunk {i} error: {exc}")
                continue
            b64 = base64.b64encode(pcm).decode("ascii")
            num_s = len(pcm) // 2
            total_samples += num_s
            total_chunks += 1
            _write({
                "streaming_chunk": True,
                "request_id": req_id,
                "chunk_index": i,
                "pcm_base64": b64,
                "sample_rate": sr,
                "num_samples": num_s,
                "num_channels": 1,
            })
            _log(f"streaming.chunk idx={i} samples={num_s} dur={num_s/sr:.2f}s")

        total_s = time.time() - t0
        _write({
            "ok": True,
            "streaming_done": True,
            "request_id": req_id,
            "sample_rate": 24000,
            "total_samples": total_samples,
            "num_channels": 1,
            "total_chunks": total_chunks,
            "metrics": {
                "request_id": req_id,
                "model_seconds": total_s,
                "total_seconds": total_s,
            },
        })
        _log(f"synthesize.done req_id={req_id} ok=1 chunks={total_chunks} total_s={total_s:.3f}")
    else:
        # Non-streaming: synthesize all at once
        try:
            pcm, sr = worker.synthesize_sentence(text, voice, speed)
        except Exception as exc:
            _write({"ok": False, "error": str(exc)})
            return
        b64 = base64.b64encode(pcm).decode("ascii")
        total_s = time.time() - t0
        _write({
            "ok": True,
            "sample_rate": sr,
            "num_samples": len(pcm) // 2,
            "num_channels": 1,
            "pcm_base64": b64,
            "metrics": {
                "request_id": req_id,
                "model_seconds": total_s,
                "total_seconds": total_s,
            },
        })
        _log(f"synthesize.done req_id={req_id} ok=1 total_s={total_s:.3f}")


# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Kokoro ONNX TTS worker for LocalTalker")
    parser.add_argument("--cache-dir", default="",
                        help="Directory for cached model files (auto-creates, auto-downloads)")
    parser.add_argument("--speed", type=float, default=1.0,
                        help="Global speech speed multiplier (default 1.0)")
    args = parser.parse_args()

    cache_dir = _resolve_cache_dir(args.cache_dir)
    _log(f"Kokoro TTS worker starting. cache_dir={cache_dir} speed={args.speed}")

    try:
        model_path, voices_path = _resolve_model_files(cache_dir)
    except Exception as exc:
        _log(f"FATAL: Could not resolve Kokoro model files: {exc}")
        sys.exit(1)

    worker: KokoroWorker | None = None

    for raw in sys.stdin:
        raw = raw.strip()
        if not raw:
            continue

        try:
            req = json.loads(raw)
        except json.JSONDecodeError as exc:
            _write({"ok": False, "error": f"JSON parse error: {exc}"})
            continue

        cmd = req.get("cmd", "")

        # ---- health --------------------------------------------------------
        if cmd == "health":
            if worker is None:
                try:
                    worker = KokoroWorker(model_path, voices_path, args.speed)
                except Exception as exc:
                    _write({"ok": False, "ready": False, "error": str(exc)})
                    continue
            _write({
                "ok": True,
                "ready": True,
                "backend": "kokoro-onnx",
                "speakers": worker.get_voices(),
                "pid": os.getpid(),
                "python_executable": sys.executable,
                "python_version": sys.version.split()[0],
                "speed": args.speed,
                "cache_dir": cache_dir,
            })

        # ---- synthesize ----------------------------------------------------
        elif cmd == "synthesize":
            if worker is None:
                try:
                    worker = KokoroWorker(model_path, voices_path, args.speed)
                except Exception as exc:
                    _write({"ok": False, "error": f"Model load failed: {exc}"})
                    continue
            try:
                _handle_synthesize(worker, req)
            except Exception as exc:
                _log(f"synthesize unhandled error: {traceback.format_exc()}")
                _write({"ok": False, "error": str(exc)})

        # ---- list_voices ---------------------------------------------------
        elif cmd == "list_voices":
            if worker is None:
                try:
                    worker = KokoroWorker(model_path, voices_path, args.speed)
                except Exception as exc:
                    _write({"ok": False, "error": str(exc)})
                    continue
            _write({"ok": True, "speakers": worker.get_voices()})

        # ---- shutdown ------------------------------------------------------
        elif cmd == "shutdown":
            _log("Kokoro TTS worker shutting down.")
            break

        else:
            _write({"ok": False, "error": f"Unknown command: {cmd}"})


if __name__ == "__main__":
    main()
