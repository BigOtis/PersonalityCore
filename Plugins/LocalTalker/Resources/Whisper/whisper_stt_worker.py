#!/usr/bin/env python3
"""LocalTalker Whisper STT worker.

Protocol: one JSON object per line over stdin/stdout.
- {"cmd":"transcribe","id":"...","audio_path":"...","model":"base.en","language":"en","vad_filter":true,"cache_dir":"..."}
- {"cmd":"shutdown"}

Response:
- {"id":"...","ok":true,"text":"..."}
- {"id":"...","ok":false,"error":"..."}
"""

from __future__ import annotations

import json
import os
import sys
import traceback
from typing import Dict, Tuple


_MODELS: Dict[Tuple[str, str], object] = {}


def _json_out(payload: dict) -> None:
    sys.stdout.write(json.dumps(payload, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def _load_model(model_name: str, cache_dir: str | None):
    key = (model_name, cache_dir or "")
    if key in _MODELS:
        return _MODELS[key]

    from faster_whisper import WhisperModel

    kwargs = {
        "model_size_or_path": model_name,
        "device": "cpu",
        "compute_type": "int8",
    }
    if cache_dir:
        kwargs["download_root"] = cache_dir

    model = WhisperModel(**kwargs)
    _MODELS[key] = model
    return model


def _handle_transcribe(req: dict) -> dict:
    req_id = str(req.get("id", ""))
    audio_path = str(req.get("audio_path", ""))
    model_name = str(req.get("model", "base.en"))
    language = str(req.get("language", "")).strip()
    vad_filter = bool(req.get("vad_filter", True))
    cache_dir = str(req.get("cache_dir", "")).strip() or None

    if not req_id:
        return {"id": "", "ok": False, "error": "Missing request id."}
    if not audio_path:
        return {"id": req_id, "ok": False, "error": "Missing audio_path."}
    if not os.path.isfile(audio_path):
        return {"id": req_id, "ok": False, "error": f"Audio file not found: {audio_path}"}

    try:
        model = _load_model(model_name, cache_dir)
        kwargs = {
            "vad_filter": vad_filter,
            "beam_size": 1,
            "temperature": 0.0,
            "condition_on_previous_text": False,
        }
        if language:
            kwargs["language"] = language

        segments, _info = model.transcribe(audio_path, **kwargs)
        text = " ".join(seg.text.strip() for seg in segments if getattr(seg, "text", "")).strip()
        return {"id": req_id, "ok": True, "text": text}
    except Exception as exc:  # pragma: no cover
        return {
            "id": req_id,
            "ok": False,
            "error": f"{type(exc).__name__}: {exc}",
        }


def main() -> int:
    for raw in sys.stdin:
        line = raw.strip()
        if not line:
            continue

        try:
            req = json.loads(line)
        except Exception:
            _json_out({"id": "", "ok": False, "error": "Invalid JSON request."})
            continue

        cmd = str(req.get("cmd", "")).strip().lower()
        if cmd == "shutdown":
            _json_out({"ok": True, "event": "bye"})
            return 0

        if cmd == "transcribe":
            _json_out(_handle_transcribe(req))
            continue

        _json_out({"id": str(req.get("id", "")), "ok": False, "error": f"Unknown command: {cmd}"})

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except SystemExit:
        raise
    except Exception:  # pragma: no cover
        traceback.print_exc(file=sys.stderr)
        raise
