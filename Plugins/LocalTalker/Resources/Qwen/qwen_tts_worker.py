#!/usr/bin/env python3
import argparse
import json
import os
import sys
from typing import Any, Dict, List, Optional

import numpy as np
import soundfile as sf
import torch

from qwen_tts import Qwen3TTSModel, VoiceClonePromptItem


def _log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


def _dtype_from_str(s: str) -> torch.dtype:
    t = (s or "").strip().lower()
    if t in ("bf16", "bfloat16"):
        return torch.bfloat16
    if t in ("fp16", "float16", "half"):
        return torch.float16
    if t in ("fp32", "float32"):
        return torch.float32
    raise ValueError(f"Unsupported dtype: {s}")


def _to_tensor(x: Any) -> Optional[torch.Tensor]:
    if x is None:
        return None
    if torch.is_tensor(x):
        return x
    return torch.tensor(x)


class Worker:
    def __init__(self, model_path: str, tokenizer_path: str, device: str, dtype: str, use_flash_attn: bool):
        load_kwargs: Dict[str, Any] = {
            "device_map": device,
            "dtype": _dtype_from_str(dtype),
        }
        if use_flash_attn:
            load_kwargs["attn_implementation"] = "flash_attention_2"

        _log(f"Loading Qwen3-TTS model: {model_path}")
        self.model = Qwen3TTSModel.from_pretrained(model_path, **load_kwargs)
        self.model_kind = getattr(self.model.model, "tts_model_type", "unknown")
        self.tokenizer_path = tokenizer_path
        if self.tokenizer_path:
            _log("Tokenizer override is currently informational; Qwen3TTSModel loads processor from model path.")
        self.voice_prompt_cache: Dict[str, Any] = {}

        try:
            self.speakers = self.model.get_supported_speakers() or []
        except Exception:
            self.speakers = []

        try:
            self.languages = self.model.get_supported_languages() or []
        except Exception:
            self.languages = []

        _log(f"Model loaded. kind={self.model_kind} speakers={len(self.speakers)} languages={len(self.languages)}")

    def _load_voice_prompt(self, path: str):
        apath = os.path.abspath(path)
        if apath in self.voice_prompt_cache:
            return self.voice_prompt_cache[apath]

        if not os.path.exists(apath):
            raise FileNotFoundError(f"Voice prompt file not found: {apath}")

        try:
            payload = torch.load(apath, map_location="cpu", weights_only=True)
        except TypeError:
            payload = torch.load(apath, map_location="cpu")

        loaded = None

        if isinstance(payload, dict) and "items" in payload and isinstance(payload["items"], list):
            items: List[VoiceClonePromptItem] = []
            for raw in payload["items"]:
                if not isinstance(raw, dict):
                    continue
                ref_code = _to_tensor(raw.get("ref_code"))
                ref_spk = _to_tensor(raw.get("ref_spk_embedding"))
                if ref_spk is None:
                    continue
                items.append(
                    VoiceClonePromptItem(
                        ref_code=ref_code,
                        ref_spk_embedding=ref_spk,
                        x_vector_only_mode=bool(raw.get("x_vector_only_mode", False)),
                        icl_mode=bool(raw.get("icl_mode", not bool(raw.get("x_vector_only_mode", False)))),
                        ref_text=raw.get("ref_text"),
                    )
                )
            if not items:
                raise ValueError(f"No valid VoiceClonePromptItem entries in: {apath}")
            loaded = items
        elif isinstance(payload, list):
            loaded = payload
        elif isinstance(payload, dict):
            loaded = payload
        else:
            raise ValueError(f"Unsupported voice prompt payload type: {type(payload)}")

        self.voice_prompt_cache[apath] = loaded
        return loaded

    def _pick_default_speaker(self) -> str:
        if self.speakers:
            return str(self.speakers[0])
        raise ValueError("No speaker provided and model did not report supported speakers.")

    def synthesize(self, req: Dict[str, Any]) -> Dict[str, Any]:
        text = str(req.get("text", "")).strip()
        if not text:
            return {"ok": False, "error": "text is required"}

        language = str(req.get("language", "Auto") or "Auto")
        speaker = req.get("speaker")
        speaker = str(speaker).strip() if speaker is not None else ""
        instruct = req.get("instruct")
        instruct = str(instruct).strip() if instruct is not None else ""
        voice_prompt_path = req.get("voice_prompt_path")
        voice_prompt_path = str(voice_prompt_path).strip() if voice_prompt_path is not None else ""
        output_wav = str(req.get("output_wav", "")).strip()
        non_streaming_mode = bool(req.get("non_streaming_mode", True))

        if not output_wav:
            return {"ok": False, "error": "output_wav is required"}

        os.makedirs(os.path.dirname(os.path.abspath(output_wav)), exist_ok=True)

        if self.model_kind == "custom_voice":
            if not speaker:
                speaker = self._pick_default_speaker()

            wavs, sr = self.model.generate_custom_voice(
                text=text,
                speaker=speaker,
                language=language,
                instruct=(instruct if instruct else None),
                non_streaming_mode=non_streaming_mode,
            )

        elif self.model_kind == "base":
            if not voice_prompt_path:
                return {
                    "ok": False,
                    "error": "voice_prompt_path is required for Base model (bring your own externally-generated prompt asset)",
                }

            voice_prompt = self._load_voice_prompt(voice_prompt_path)
            wavs, sr = self.model.generate_voice_clone(
                text=text,
                language=language,
                voice_clone_prompt=voice_prompt,
                non_streaming_mode=non_streaming_mode,
            )

        elif self.model_kind == "voice_design":
            return {
                "ok": False,
                "error": "voice_design models are intentionally unsupported in LocalTalker runtime. Bring externally-generated voice prompts or use CustomVoice speakers.",
            }

        else:
            return {"ok": False, "error": f"Unsupported model kind: {self.model_kind}"}

        wav = np.asarray(wavs[0], dtype=np.float32)
        sf.write(output_wav, wav, int(sr), subtype="PCM_16")

        return {
            "ok": True,
            "wav_path": os.path.abspath(output_wav),
            "sample_rate": int(sr),
            "num_samples": int(wav.shape[0]),
        }


def _write_response(resp: Dict[str, Any]) -> None:
    print(json.dumps(resp, ensure_ascii=False), flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="LocalTalker Qwen3-TTS worker")
    parser.add_argument("--model", required=True, help="Qwen model repo id or local path")
    parser.add_argument("--tokenizer", default="", help="Optional tokenizer repo id or local path")
    parser.add_argument("--device", default="cuda:0", help="Device map (cpu, cuda, cuda:0, ...)")
    parser.add_argument("--dtype", default="bfloat16", help="bfloat16/float16/float32")
    parser.add_argument("--flash-attn", dest="flash_attn", action="store_true")
    parser.add_argument("--no-flash-attn", dest="flash_attn", action="store_false")
    parser.set_defaults(flash_attn=False)
    args = parser.parse_args()

    try:
        worker = Worker(
            model_path=args.model,
            tokenizer_path=args.tokenizer,
            device=args.device,
            dtype=args.dtype,
            use_flash_attn=bool(args.flash_attn),
        )
    except Exception as exc:
        _write_response({"ok": False, "error": f"failed_to_initialize: {exc}"})
        return 2

    for raw in sys.stdin:
        line = (raw or "").strip()
        if not line:
            continue

        try:
            req = json.loads(line)
            cmd = str(req.get("cmd", "")).strip().lower()

            if cmd == "health":
                _write_response(
                    {
                        "ok": True,
                        "ready": True,
                        "model_kind": worker.model_kind,
                        "speakers": worker.speakers,
                        "languages": worker.languages,
                    }
                )
                continue

            if cmd == "list_voices":
                _write_response({"ok": True, "voices": worker.speakers})
                continue

            if cmd == "synthesize":
                _write_response(worker.synthesize(req))
                continue

            if cmd == "shutdown":
                _write_response({"ok": True, "shutdown": True})
                break

            _write_response({"ok": False, "error": f"unknown_command: {cmd}"})

        except Exception as exc:
            _write_response({"ok": False, "error": str(exc)})

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
