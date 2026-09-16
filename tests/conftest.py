from __future__ import annotations

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
src = ROOT / "src"
if str(src) not in sys.path:
    sys.path.insert(0, str(src))

from localtalker.config import AppConfig
from localtalker.inference import InferenceProvider, InferenceProviderKind
from localtalker.models import VoiceInfo
from localtalker.speech import SpeechChunk, TranscriptResult


class FakeSpeech:
    def __init__(self) -> None:
        self.received_text: list[str] = []
        self.transcripts: list[str] = ["hello there"]

    def status(self) -> dict:
        return {
            "stt": {"model": "fake", "device": "cpu", "loaded": True},
            "tts": {"engine": "fake", "loaded": True, "enabled": True},
            "error": None,
        }

    def list_voices(self) -> list[dict]:
        return [VoiceInfo(id="af_bella", name="Bella", gender="female", traits="soft").model_dump()]

    async def warmup(self) -> None:
        return None

    async def transcribe_pcm16(self, pcm16: bytes, sample_rate: int, channels: int = 1) -> TranscriptResult:
        text = self.transcripts.pop(0) if self.transcripts else "hello there"
        return TranscriptResult(text=text, duration_ms=18)

    async def synthesize(self, text: str, voice_id: str, speed: float = 1.0):
        chunks = []
        async for chunk in self.synthesize_stream(text, voice_id, speed):
            chunks.append(chunk)
        return chunks

    async def synthesize_stream(self, text: str, voice_id: str, speed: float = 1.0):
        self.received_text.append(text)
        yield SpeechChunk(pcm16=b"\x00\x00" * 160, sample_rate=24000, index=0)


@pytest.fixture
def mock_config() -> AppConfig:
    return AppConfig(
        preferred_provider_id="mock",
        providers=[
            InferenceProvider(
                id="mock",
                name="Mock",
                kind=InferenceProviderKind.mock,
                model="mock-character",
                enabled=True,
            )
        ],
    )
