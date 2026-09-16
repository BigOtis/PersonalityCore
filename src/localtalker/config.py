from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Literal

from pydantic import BaseModel, Field

from localtalker.models import InferenceProvider, InferenceProviderKind
from localtalker.paths import config_path


class SpeechConfig(BaseModel):
    # Portable game bundles can point at immutable, preinstalled assets.
    kokoro_model_path: str = ""
    kokoro_voices_path: str = ""
    whisper_model: str = "base.en"
    whisper_device: Literal["auto", "cpu", "cuda"] = "auto"
    whisper_language: str = "en"
    cuda_library_path: str = ""
    tts_enabled: bool = True
    listen_mode: Literal["push_to_talk", "auto"] = "push_to_talk"


class AppConfig(BaseModel):
    host: str = "127.0.0.1"
    port: int = Field(default=8765, ge=1, le=65535)
    providers: list[InferenceProvider] = Field(default_factory=list)
    speech: SpeechConfig = Field(default_factory=SpeechConfig)
    llama_server_path: str = ""
    llama_model_path: str = ""
    llama_port: int = Field(default=8090, ge=1, le=65535)
    llama_n_gpu_layers: int = 99
    llama_ctx: int = Field(default=4096, ge=512, le=131072)
    llama_flash_attn: bool = True
    llama_jinja: bool = True
    preferred_provider_id: str = ""

    def provider(self, provider_id: str | None) -> InferenceProvider | None:
        if provider_id:
            for item in self.providers:
                if item.id == provider_id:
                    return item
        if self.preferred_provider_id:
            for item in self.providers:
                if item.id == self.preferred_provider_id:
                    return item
        return None


def default_providers() -> list[InferenceProvider]:
    return [
        InferenceProvider(
            id="mock",
            name="Mock (tests / offline)",
            kind=InferenceProviderKind.mock,
            model="mock-character",
            enabled=True,
        ),
        InferenceProvider(
            id="ollama",
            name="Ollama",
            kind=InferenceProviderKind.ollama,
            base_url="http://127.0.0.1:11434/v1",
            model="",
            enabled=True,
        ),
        InferenceProvider(
            id="llamacpp",
            name="llama.cpp server",
            kind=InferenceProviderKind.llama_cpp,
            base_url="http://127.0.0.1:8090/v1",
            model="",
            enabled=True,
            extra={"direct_speech": True},
        ),
        InferenceProvider(
            id="openai-compat",
            name="OpenAI-compatible",
            kind=InferenceProviderKind.openai_compat,
            base_url="http://127.0.0.1:1234/v1",
            model="",
            enabled=True,
        ),
    ]


def load_config(path: Path | None = None) -> AppConfig:
    path = path or config_path()
    if path.exists():
        data = json.loads(path.read_text(encoding="utf-8"))
        cfg = AppConfig.model_validate(data)
    else:
        cfg = AppConfig(providers=default_providers())
        save_config(cfg, path)
        return cfg
    if not cfg.providers:
        cfg.providers = default_providers()
    known = {p.id for p in cfg.providers}
    for item in default_providers():
        if item.id not in known:
            cfg.providers.append(item)
    return cfg


def save_config(cfg: AppConfig, path: Path | None = None) -> None:
    path = path or config_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(cfg.model_dump_json(indent=2), encoding="utf-8")


def merge_config(cfg: AppConfig, patch: dict[str, Any]) -> AppConfig:
    data = cfg.model_dump()
    patch = dict(patch)
    if "speech" in patch:
        patch["speech"] = {**data["speech"], **patch["speech"]}
    data.update(patch)
    updated = AppConfig.model_validate(data)
    save_config(updated)
    return updated
