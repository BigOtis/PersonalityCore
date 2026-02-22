#!/usr/bin/env python3
import argparse
from collections import OrderedDict
import json
import os
import sys
import time
import wave
from typing import Any, Dict, List, Optional

import numpy as np
import torch

from qwen_tts import Qwen3TTSModel, VoiceClonePromptItem

try:
    import soundfile as sf
except Exception:
    sf = None


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


def _opt_int(req: Dict[str, Any], key: str) -> Optional[int]:
    v = req.get(key)
    if v is None:
        return None
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, (int, float)):
        return int(v)
    s = str(v).strip()
    if not s:
        return None
    return int(float(s))


def _opt_float(req: Dict[str, Any], key: str) -> Optional[float]:
    v = req.get(key)
    if v is None:
        return None
    if isinstance(v, bool):
        return float(int(v))
    if isinstance(v, (int, float)):
        return float(v)
    s = str(v).strip()
    if not s:
        return None
    return float(s)


def _opt_bool(req: Dict[str, Any], key: str) -> Optional[bool]:
    v = req.get(key)
    if v is None:
        return None
    if isinstance(v, bool):
        return v
    if isinstance(v, (int, float)):
        return bool(v)
    s = str(v).strip().lower()
    if not s:
        return None
    if s in ("1", "true", "yes", "on"):
        return True
    if s in ("0", "false", "no", "off"):
        return False
    raise ValueError(f"Invalid bool value for {key}: {v}")


def _write_wav_pcm16(path: str, wav: np.ndarray, sample_rate: int) -> None:
    # Convert float waveform to little-endian PCM16 without external deps.
    interleaved, channels, num_samples = _to_pcm16_interleaved_bytes(wav)
    _write_wav_pcm16_bytes(path, interleaved, sample_rate, channels)


def _to_pcm16_interleaved_bytes(wav: np.ndarray) -> tuple[bytes, int, int]:
    arr = np.asarray(wav)
    if arr.ndim == 1:
        channels = 1
        pcm = np.clip(arr, -1.0, 1.0)
        pcm_i16 = (pcm * 32767.0).astype(np.int16)
        interleaved = pcm_i16
        num_samples = int(arr.shape[0])
    elif arr.ndim == 2:
        # Accept either [channels, samples] or [samples, channels].
        if arr.shape[0] <= 8 and arr.shape[0] < arr.shape[1]:
            arr = arr.T
        channels = int(arr.shape[1])
        pcm = np.clip(arr, -1.0, 1.0)
        pcm_i16 = (pcm * 32767.0).astype(np.int16)
        interleaved = pcm_i16.reshape(-1)
        num_samples = int(arr.shape[0])
    else:
        raise ValueError(f"Unsupported wav shape for PCM16 write: {arr.shape}")
    return interleaved.tobytes(), channels, num_samples


def _write_wav_pcm16_bytes(path: str, pcm_i16_le_bytes: bytes, sample_rate: int, channels: int) -> None:
    with wave.open(path, "wb") as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(2)
        wf.setframerate(int(sample_rate))
        wf.writeframes(pcm_i16_le_bytes)


class Worker:
    def __init__(self, model_path: str, tokenizer_path: str, device: str, dtype: str, use_flash_attn: bool, cache_size: int):
        resolved_device = device
        resolved_dtype = dtype
        if str(device).strip().lower().startswith("cuda") and not torch.cuda.is_available():
            _log(f"Requested device '{device}' but CUDA is unavailable; falling back to CPU/float32.")
            resolved_device = "cpu"
            resolved_dtype = "float32"

        load_kwargs: Dict[str, Any] = {
            "device_map": resolved_device,
            "dtype": _dtype_from_str(resolved_dtype),
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
        self.cache_size = max(0, int(cache_size))
        self.synth_cache: "OrderedDict[str, Dict[str, Any]]" = OrderedDict()

        try:
            self.speakers = self.model.get_supported_speakers() or []
        except Exception:
            self.speakers = []

        try:
            self.languages = self.model.get_supported_languages() or []
        except Exception:
            self.languages = []

        _log(f"Model loaded. kind={self.model_kind} speakers={len(self.speakers)} languages={len(self.languages)}")

    def _cache_key(self, req: Dict[str, Any], text: str, language: str, speaker: str, instruct: str, voice_prompt_path: str, non_streaming_mode: bool, gen_kwargs: Dict[str, Any]) -> str:
        key_obj = {
            "mk": self.model_kind,
            "text": text,
            "language": language,
            "speaker": speaker,
            "instruct": instruct,
            "voice_prompt_path": os.path.abspath(voice_prompt_path) if voice_prompt_path else "",
            "non_streaming_mode": bool(non_streaming_mode),
            "gen_kwargs": gen_kwargs,
        }
        return json.dumps(key_obj, ensure_ascii=False, sort_keys=True, separators=(",", ":"))

    def _cache_get(self, key: str) -> Optional[Dict[str, Any]]:
        if self.cache_size <= 0:
            return None
        hit = self.synth_cache.get(key)
        if hit is None:
            return None
        self.synth_cache.move_to_end(key)
        return hit

    def _cache_put(self, key: str, value: Dict[str, Any]) -> None:
        if self.cache_size <= 0:
            return
        self.synth_cache[key] = value
        self.synth_cache.move_to_end(key)
        while len(self.synth_cache) > self.cache_size:
            self.synth_cache.popitem(last=False)

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
        t_all0 = time.perf_counter()
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

        gen_kwargs: Dict[str, Any] = {}
        for key in ("max_new_tokens", "top_k"):
            value = _opt_int(req, key)
            if value is not None:
                gen_kwargs[key] = value

        for key in ("top_p", "temperature", "repetition_penalty"):
            value = _opt_float(req, key)
            if value is not None:
                gen_kwargs[key] = value

        for key in ("do_sample", "subtalker_dosample"):
            value = _opt_bool(req, key)
            if value is not None:
                gen_kwargs[key] = value

        use_cache = bool(req.get("use_cache", True))
        cache_key = self._cache_key(req, text, language, speaker, instruct, voice_prompt_path, non_streaming_mode, gen_kwargs)
        if use_cache:
            cached = self._cache_get(cache_key)
            if cached is not None:
                t_write0 = time.perf_counter()
                _write_wav_pcm16_bytes(
                    output_wav,
                    cached["pcm16_bytes"],
                    int(cached["sample_rate"]),
                    int(cached["channels"]),
                )
                t_write1 = time.perf_counter()
                return {
                    "ok": True,
                    "wav_path": os.path.abspath(output_wav),
                    "sample_rate": int(cached["sample_rate"]),
                    "num_samples": int(cached["num_samples"]),
                    "metrics": {
                        "cache_hit": True,
                        "model_seconds": 0.0,
                        "write_seconds": round(t_write1 - t_write0, 6),
                        "total_seconds": round(time.perf_counter() - t_all0, 6),
                    },
                }

        t_model0 = time.perf_counter()
        if self.model_kind == "custom_voice":
            if not speaker:
                speaker = self._pick_default_speaker()

            wavs, sr = self.model.generate_custom_voice(
                text=text,
                speaker=speaker,
                language=language,
                instruct=(instruct if instruct else None),
                non_streaming_mode=non_streaming_mode,
                **gen_kwargs,
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
                **gen_kwargs,
            )

        elif self.model_kind == "voice_design":
            return {
                "ok": False,
                "error": "voice_design models are intentionally unsupported in LocalTalker runtime. Bring externally-generated voice prompts or use CustomVoice speakers.",
            }

        else:
            return {"ok": False, "error": f"Unsupported model kind: {self.model_kind}"}

        wav = np.asarray(wavs[0], dtype=np.float32)
        t_model1 = time.perf_counter()

        pcm_bytes, channels, num_samples = _to_pcm16_interleaved_bytes(wav)
        self._cache_put(
            cache_key,
            {
                "pcm16_bytes": pcm_bytes,
                "sample_rate": int(sr),
                "channels": int(channels),
                "num_samples": int(num_samples),
            },
        )

        t_write0 = time.perf_counter()
        _write_wav_pcm16_bytes(output_wav, pcm_bytes, int(sr), int(channels))
        t_write1 = time.perf_counter()

        return {
            "ok": True,
            "wav_path": os.path.abspath(output_wav),
            "sample_rate": int(sr),
            "num_samples": int(num_samples),
            "metrics": {
                "cache_hit": False,
                "model_seconds": round(t_model1 - t_model0, 6),
                "write_seconds": round(t_write1 - t_write0, 6),
                "total_seconds": round(time.perf_counter() - t_all0, 6),
            },
        }

    def synthesize_batch(self, req: Dict[str, Any]) -> Dict[str, Any]:
        t_all0 = time.perf_counter()
        items = req.get("items")
        if not isinstance(items, list) or len(items) == 0:
            return {"ok": False, "error": "items is required and must be a non-empty list"}

        # Current batch path supports CustomVoice only.
        if self.model_kind != "custom_voice":
            return {"ok": False, "error": f"synthesize_batch currently supports custom_voice only (model_kind={self.model_kind})"}

        parsed: List[Dict[str, Any]] = []
        texts: List[str] = []
        speakers: List[str] = []
        languages: List[str] = []
        instructs: List[Optional[str]] = []

        for i, item in enumerate(items):
            if not isinstance(item, dict):
                return {"ok": False, "error": f"items[{i}] must be an object"}
            text = str(item.get("text", "")).strip()
            if not text:
                return {"ok": False, "error": f"items[{i}].text is required"}
            output_wav = str(item.get("output_wav", "")).strip()
            if not output_wav:
                return {"ok": False, "error": f"items[{i}].output_wav is required"}
            language = str(item.get("language", "Auto") or "Auto")
            speaker = str(item.get("speaker", "") or "").strip()
            if not speaker:
                speaker = self._pick_default_speaker()
            instruct = str(item.get("instruct", "") or "").strip()
            os.makedirs(os.path.dirname(os.path.abspath(output_wav)), exist_ok=True)

            parsed.append(
                {
                    "text": text,
                    "output_wav": output_wav,
                    "language": language,
                    "speaker": speaker,
                    "instruct": instruct,
                }
            )
            texts.append(text)
            languages.append(language)
            speakers.append(speaker)
            instructs.append(instruct if instruct else None)

        gen_kwargs: Dict[str, Any] = {}
        for key in ("max_new_tokens", "top_k"):
            value = _opt_int(req, key)
            if value is not None:
                gen_kwargs[key] = value
        for key in ("top_p", "temperature", "repetition_penalty"):
            value = _opt_float(req, key)
            if value is not None:
                gen_kwargs[key] = value
        for key in ("do_sample", "subtalker_dosample"):
            value = _opt_bool(req, key)
            if value is not None:
                gen_kwargs[key] = value

        t_model0 = time.perf_counter()
        wavs, sr = self.model.generate_custom_voice(
            text=texts,
            speaker=speakers,
            language=languages,
            instruct=instructs,
            non_streaming_mode=True,
            **gen_kwargs,
        )
        t_model1 = time.perf_counter()

        out_items: List[Dict[str, Any]] = []
        t_write_total = 0.0
        for item, wav in zip(parsed, wavs):
            t_w0 = time.perf_counter()
            pcm_bytes, channels, num_samples = _to_pcm16_interleaved_bytes(np.asarray(wav, dtype=np.float32))
            _write_wav_pcm16_bytes(item["output_wav"], pcm_bytes, int(sr), int(channels))
            t_w1 = time.perf_counter()
            t_write_total += (t_w1 - t_w0)
            out_items.append(
                {
                    "ok": True,
                    "wav_path": os.path.abspath(item["output_wav"]),
                    "sample_rate": int(sr),
                    "num_samples": int(num_samples),
                    "speaker": item["speaker"],
                    "language": item["language"],
                }
            )

        return {
            "ok": True,
            "items": out_items,
            "metrics": {
                "model_seconds": round(t_model1 - t_model0, 6),
                "write_seconds": round(t_write_total, 6),
                "total_seconds": round(time.perf_counter() - t_all0, 6),
                "batch_size": len(out_items),
            },
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
    parser.add_argument("--cache-size", type=int, default=64, help="Number of recent synth results kept in memory cache")
    parser.set_defaults(flash_attn=False)
    args = parser.parse_args()

    try:
        worker = Worker(
            model_path=args.model,
            tokenizer_path=args.tokenizer,
            device=args.device,
            dtype=args.dtype,
            use_flash_attn=bool(args.flash_attn),
            cache_size=max(0, int(args.cache_size)),
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

            if cmd == "synthesize_batch":
                _write_response(worker.synthesize_batch(req))
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
