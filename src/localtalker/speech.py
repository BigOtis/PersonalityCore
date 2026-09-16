from __future__ import annotations

import asyncio
import math
import os
import threading
import base64
import io
import logging
import re
import urllib.request
import wave
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np

from localtalker.config import SpeechConfig
from localtalker.paths import kokoro_dir, whisper_dir

log = logging.getLogger("localtalker.speech")

KOKORO_MODEL = "kokoro-v1.0.onnx"
KOKORO_VOICES = "voices-v1.0.bin"
KOKORO_BASE = "https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0"

KNOWN_VOICES = [
    ("af_alloy", "Alloy", "female", "clear"),
    ("af_aoede", "Aoede", "female", "warm"),
    ("af_bella", "Bella", "female", "soft"),
    ("af_heart", "Heart", "female", "bright"),
    ("af_jessica", "Jessica", "female", "calm"),
    ("af_kore", "Kore", "female", "firm"),
    ("af_nicole", "Nicole", "female", "playful"),
    ("af_nova", "Nova", "female", "crisp"),
    ("af_river", "River", "female", "gentle"),
    ("af_sarah", "Sarah", "female", "steady"),
    ("af_sky", "Sky", "female", "light"),
    ("am_adam", "Adam", "male", "neutral"),
    ("am_echo", "Echo", "male", "low"),
    ("am_eric", "Eric", "male", "clear"),
    ("am_fenrir", "Fenrir", "male", "dark"),
    ("am_liam", "Liam", "male", "soft"),
    ("am_michael", "Michael", "male", "grounded"),
    ("am_onyx", "Onyx", "male", "deep"),
    ("am_puck", "Puck", "male", "lively"),
    ("bf_emma", "Emma", "female", "british"),
    ("bf_isabella", "Isabella", "female", "british"),
    ("bm_george", "George", "male", "british"),
    ("bm_lewis", "Lewis", "male", "british"),
]

_SENTENCE = re.compile(r"(?<=[.!?])\s+")


@dataclass
class TranscriptResult:
    text: str
    duration_ms: float


@dataclass
class SpeechChunk:
    pcm16: bytes
    sample_rate: int
    index: int


class SpeechService:
    def __init__(self, cfg: SpeechConfig):
        self.cfg = cfg
        self._whisper = None
        self._kokoro = None
        self._whisper_device = "cpu"
        self._ready_error: str | None = None
        self._whisper_lock = threading.RLock()
        self._kokoro_lock = threading.RLock()
        self._dll_dirs = []
        if os.name == "nt" and cfg.cuda_library_path:
            directory = Path(cfg.cuda_library_path).resolve()
            if directory.is_dir():
                self._dll_dirs.append(os.add_dll_directory(str(directory)))
                os.environ["PATH"] = str(directory) + os.pathsep + os.environ.get("PATH", "")

    def status(self) -> dict[str, Any]:
        return {
            "stt": {
                "model": self.cfg.whisper_model,
                "device": self._whisper_device,
                "loaded": self._whisper is not None,
            },
            "tts": {
                "engine": "kokoro-onnx",
                "loaded": self._kokoro is not None,
                "enabled": self.cfg.tts_enabled,
            },
            "error": self._ready_error,
        }

    def list_voices(self) -> list[dict[str, str]]:
        ids: list[str] = []
        if self._kokoro is not None:
            try:
                ids = list(self._kokoro.get_voices())
            except Exception:
                ids = []
        known = {item[0]: item for item in KNOWN_VOICES}
        if not ids:
            ids = [item[0] for item in KNOWN_VOICES]
        voices = []
        for voice_id in ids:
            meta = known.get(voice_id, (voice_id, voice_id.replace("_", " ").title(), "", ""))
            voices.append(
                {
                    "id": voice_id,
                    "name": meta[1],
                    "language": "en-us" if not voice_id.startswith("b") else "en-gb",
                    "gender": meta[2],
                    "traits": meta[3],
                }
            )
        return voices

    async def warmup(self) -> None:
        async def stt():
            try:
                await asyncio.to_thread(self._load_whisper)
                await asyncio.to_thread(self._prime_whisper)
            except Exception as exc:
                self._ready_error = f"Speech recognition unavailable: {exc}"
                log.warning(self._ready_error)
        async def tts():
            if self.cfg.tts_enabled:
                try:
                    await asyncio.to_thread(self._load_kokoro)
                except Exception as exc:
                    self._ready_error = f"Voice synthesis unavailable: {exc}"
                    log.warning(self._ready_error)
        await asyncio.gather(stt(), tts())

    def _prime_whisper(self):
        # Initialize CUDA kernels in the background, before the first microphone turn.
        with self._whisper_lock:
            try:
                segments, _ = self._whisper.transcribe(np.zeros(16000, dtype=np.float32),
                    language=self.cfg.whisper_language or "en", vad_filter=False, beam_size=1)
                list(segments)
            except RuntimeError:
                if self.cfg.whisper_device == "auto" and self._whisper_device == "cuda":
                    self._fallback_whisper_cpu()
                else:
                    raise

    def _load_whisper(self) -> None:
        with self._whisper_lock:
            if self._whisper is not None:
                return
            from faster_whisper import WhisperModel

            device, compute = _whisper_device(self.cfg.whisper_device)
            log.info("Loading Whisper %s on %s/%s", self.cfg.whisper_model, device, compute)
            self._whisper = WhisperModel(
                self.cfg.whisper_model,
                device=device,
                compute_type=compute,
                download_root=str(whisper_dir()),
            )
            self._whisper_device = device

    def _load_kokoro(self) -> None:
        with self._kokoro_lock:
            if self._kokoro is not None:
                return
            from kokoro_onnx import Kokoro

            if self.cfg.kokoro_model_path or self.cfg.kokoro_voices_path:
                model_path, voices_path = Path(self.cfg.kokoro_model_path), Path(self.cfg.kokoro_voices_path)
                if not model_path.is_file() or not voices_path.is_file():
                    raise FileNotFoundError("Bundled Kokoro model or voice data is missing. Extract the complete demo folder.")
            else:
                model_path, voices_path = ensure_kokoro_files()
            log.info("Loading Kokoro from %s", model_path)
            self._kokoro = Kokoro(str(model_path), str(voices_path))

    async def transcribe_pcm16(self, pcm16: bytes, sample_rate: int, channels: int = 1) -> TranscriptResult:
        if not pcm16:
            return TranscriptResult(text="", duration_ms=0)
        samples = pcm16_to_float32_16k(pcm16, sample_rate, channels)
        return await self.transcribe_samples(samples)

    async def transcribe_samples(self, samples: np.ndarray) -> TranscriptResult:
        if self._whisper is None:
            await asyncio.to_thread(self._load_whisper)
        try:
            return await asyncio.to_thread(self._transcribe_sync, samples)
        except RuntimeError as exc:
            if self.cfg.whisper_device != "auto" or self._whisper_device != "cuda":
                raise
            log.warning("Whisper CUDA unavailable; retrying on CPU: %s", exc)
            await asyncio.to_thread(self._fallback_whisper_cpu)
            return await asyncio.to_thread(self._transcribe_sync, samples)

    def _fallback_whisper_cpu(self):
        from faster_whisper import WhisperModel
        with self._whisper_lock:
            self._whisper = WhisperModel(self.cfg.whisper_model, device="cpu", compute_type="int8",
                                         download_root=str(whisper_dir()))
            self._whisper_device = "cpu"
            self._ready_error = "Whisper is using CPU because CUDA libraries are unavailable."


    def _transcribe_sync(self, samples: np.ndarray) -> TranscriptResult:
        with self._whisper_lock:
            import time

            started = time.perf_counter()
            if samples.size == 0:
                return TranscriptResult(text="", duration_ms=0)
            segments, _info = self._whisper.transcribe(
                samples,
                language=self.cfg.whisper_language or None,
                vad_filter=True,
                beam_size=1,
            )
            text = " ".join(segment.text.strip() for segment in segments).strip()
            return TranscriptResult(text=text, duration_ms=(time.perf_counter() - started) * 1000)

    async def synthesize(self, text: str, voice_id: str, speed: float = 1.0) -> list[SpeechChunk]:
        chunks: list[SpeechChunk] = []
        async for chunk in self.synthesize_stream(text, voice_id, speed):
            chunks.append(chunk)
        return chunks

    async def synthesize_stream(self, text: str, voice_id: str, speed: float = 1.0):
        if not text.strip():
            return
        if self._kokoro is None:
            await asyncio.to_thread(self._load_kokoro)
        sentences = split_speakable(text)
        for index, sentence in enumerate(sentences):
            pcm, rate = await asyncio.to_thread(self._synth_sentence, sentence, voice_id, speed)
            yield SpeechChunk(pcm16=pcm, sample_rate=rate, index=index)

    def _synth_sentence(self, text: str, voice_id: str, speed: float) -> tuple[bytes, int]:
        with self._kokoro_lock:
            available = []
            try:
                available = list(self._kokoro.get_voices())
            except Exception:
                available = [item[0] for item in KNOWN_VOICES]
            if voice_id not in available:
                voice_id = "af_bella" if "af_bella" in available else available[0]
            samples, rate = self._kokoro.create(text, voice=voice_id, speed=speed, lang="en-gb" if voice_id.startswith("b") else "en-us")
            pcm = (np.clip(samples, -1.0, 1.0) * 32767.0).astype(np.int16).tobytes()
            return pcm, int(rate)



def split_speakable(text: str) -> list[str]:
    parts = [part.strip() for part in _SENTENCE.split(text.strip()) if part.strip()]
    return parts or [text.strip()]


def pcm16_to_float32_16k(pcm16: bytes, sample_rate: int, channels: int) -> np.ndarray:
    from scipy.signal import resample_poly
    if channels not in (1, 2) or not 8000 <= sample_rate <= 192000:
        raise ValueError("Expected mono/stereo PCM16 at 8?192 kHz")
    if len(pcm16) % (2 * channels):
        raise ValueError("Incomplete PCM16 audio frame")
    samples = np.frombuffer(pcm16, dtype="<i2").astype(np.float32).reshape(-1, channels).mean(axis=1) / 32768.0
    if sample_rate != 16000:
        divisor = math.gcd(sample_rate, 16000)
        samples = resample_poly(samples, 16000 // divisor, sample_rate // divisor)
    return samples.astype(np.float32)


def wav_bytes_from_pcm16(pcm16: bytes, sample_rate: int) -> bytes:
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(pcm16)
    return buffer.getvalue()


def encode_pcm16(pcm16: bytes) -> str:
    return base64.b64encode(pcm16).decode("ascii")


def decode_pcm16(payload: str) -> bytes:
    return base64.b64decode(payload, validate=True)


def ensure_kokoro_files() -> tuple[Path, Path]:
    folder = kokoro_dir()
    model = folder / KOKORO_MODEL
    voices = folder / KOKORO_VOICES
    for name, dest in ((KOKORO_MODEL, model), (KOKORO_VOICES, voices)):
        if dest.is_file() and dest.stat().st_size > 0:
            continue
        url = f"{KOKORO_BASE}/{name}"
        log.info("Downloading %s", url)
        tmp = dest.with_suffix(dest.suffix + ".tmp")
        urllib.request.urlretrieve(url, tmp)
        tmp.replace(dest)
    return model, voices


def _whisper_device(requested: str) -> tuple[str, str]:
    if requested in {"cpu", "cuda"}:
        return requested, "int8" if requested == "cpu" else "float16"
    try:
        import ctranslate2

        if ctranslate2.get_cuda_device_count() > 0:
            return "cuda", "float16"
    except Exception:
        pass
    return "cpu", "int8"
