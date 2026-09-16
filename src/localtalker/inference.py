from __future__ import annotations

import asyncio
import logging
import json
import os
import shutil
import time
from collections.abc import AsyncIterator
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import httpx
from openai import APIError, BadRequestError, AsyncOpenAI

from localtalker.config import AppConfig
from localtalker.models import InferenceProvider, InferenceProviderKind
from localtalker.paths import extra_data_dirs, models_dir, runtimes_dir
from localtalker.prompts import REPLY_SCHEMA, reply_schema_for_messages

log = logging.getLogger("localtalker.inference")


@dataclass
class InferenceChunk:
    delta: str
    done: bool = False


@dataclass
class Discovery:
    reachable: bool
    models: list[str] = field(default_factory=list)
    error: str | None = None
    latency_ms: float | None = None


class InferenceError(RuntimeError):
    pass


class InferenceBackend:
    provider: InferenceProvider

    async def stream(self, messages: list[dict[str, Any]], cancel: asyncio.Event) -> AsyncIterator[InferenceChunk]:
        raise NotImplementedError

    async def close(self) -> None:
        return None


class MockBackend(InferenceBackend):
    def __init__(self, provider: InferenceProvider):
        self.provider = provider

    async def stream(self, messages: list[dict[str, Any]], cancel: asyncio.Event) -> AsyncIterator[InferenceChunk]:
        user = ""
        for message in reversed(messages):
            if message.get("role") == "user":
                user = str(message.get("content") or "")
                break
        reply = (
            '{"dialogue":"I hear you. '
            + _json_escape(user or "Go on.")
            + '","emotion":"attentive","intent":"acknowledge",'
            + '"actions":[{"name":"nod"}],"animation":"talk_idle",'
            + '"world_interactions":[],"tool_requests":[],"state_changes":{}}'
        )
        for token in _chunk_text(reply, 24):
            if cancel.is_set():
                break
            await asyncio.sleep(0)
            yield InferenceChunk(delta=token)
        yield InferenceChunk(delta="", done=True)


class OpenAICompatBackend(InferenceBackend):
    def __init__(self, provider: InferenceProvider):
        self.provider = provider
        if not provider.base_url:
            raise InferenceError(f"Provider {provider.id} has no base_url")
        self.client = AsyncOpenAI(
            base_url=provider.base_url.rstrip("/"),
            api_key=provider.api_key or "local",
            timeout=httpx.Timeout(90.0, connect=3.0),
            max_retries=0,
        )

    async def stream(self, messages: list[dict[str, Any]], cancel: asyncio.Event) -> AsyncIterator[InferenceChunk]:
        model = self.provider.model
        if not model:
            raise InferenceError(f"Provider {self.provider.id} has no model selected")
        kwargs: dict[str, Any] = {
            "model": model,
            "messages": messages,
            "temperature": self.provider.temperature,
            "max_tokens": self.provider.max_tokens,
            "stream": True,
        }
        extra = dict(self.provider.extra or {})
        direct_speech = extra.pop("direct_speech", False)
        if direct_speech and self.provider.kind == InferenceProviderKind.llama_cpp and "glimmer" in model.lower():
            async for chunk in self._stream_direct(messages, cancel, extra):
                yield chunk
            return
        if extra.pop("json_schema", True):
            kwargs["response_format"] = {
                "type": "json_schema",
                "json_schema": {"name": "character_reply", "schema": reply_schema_for_messages(messages), "strict": False},
            }
        if "glimmer" in (self.provider.model or "").lower():
            extra.setdefault("chat_template_kwargs", {"reasoning_strength": "low"})
        extra_body = {key: value for key, value in extra.items() if value is not None}
        if extra_body:
            kwargs["extra_body"] = extra_body
        try:
            stream = await self.client.chat.completions.create(**kwargs)
        except BadRequestError as exc:
            # Some local servers reject json_schema; retry as plain JSON / unconstrained.
            if "response_format" in kwargs:
                log.warning("Structured response_format rejected by %s: %s", self.provider.id, exc)
                kwargs.pop("response_format", None)
                try:
                    retry_body = dict(kwargs.get("extra_body") or {})
                    retry_body["format"] = "json"
                    stream = await self.client.chat.completions.create(**{**kwargs, "extra_body": retry_body})
                except APIError:
                    stream = await self.client.chat.completions.create(**kwargs)
            else:
                raise InferenceError(str(exc)) from exc
        try:
            async for event in stream:
                if cancel.is_set():
                    break
                choice = event.choices[0] if event.choices else None
                delta = (choice.delta.content if choice and choice.delta else None) or ""
                if delta:
                    yield InferenceChunk(delta=delta)
            yield InferenceChunk(delta="", done=True)
        finally:
            await stream.close()

    async def _stream_direct(self, messages, cancel, extra):
        """Muse speech with a hard JSON grammar, bypassing its chat parser.

        llama.cpp b10610's Muse handler ignores response_format schemas. The
        native completion endpoint enforces the same schema on every token.
        """
        root = self.provider.base_url.rstrip("/")
        if root.endswith("/v1"):
            root = root[:-3]
        template_kwargs = {"reasoning_strength": "low", **extra.pop("chat_template_kwargs", {})}
        extra.pop("json_schema", None)
        headers = {"Authorization": f"Bearer {self.provider.api_key}"} if self.provider.api_key else {}
        async with httpx.AsyncClient(base_url=root + "/", headers=headers,
                timeout=httpx.Timeout(90.0, connect=3.0)) as client:
            bounded = list(messages)
            companion = reply_schema_for_messages(messages) is not REPLY_SCHEMA
            context_limit = None
            if companion:
                props = await client.get("props")
                props.raise_for_status()
                settings = props.json().get("default_generation_settings", {})
                context_limit = settings.get("n_ctx") or props.json().get("n_ctx") or 4096
            while True:
                rendered = await client.post("apply-template", json={
                    "messages": [*bounded, {"role": "assistant", "content": " "}],
                    "chat_template_kwargs": template_kwargs,
                    "continue_final_message": "content", "add_generation_prompt": False,
                })
                rendered.raise_for_status()
                prompt = rendered.json()["prompt"]
                if not companion:
                    break
                tokenized = await client.post("tokenize", json={"content": prompt, "add_special": True})
                tokenized.raise_for_status()
                token_count = len(tokenized.json()["tokens"])
                if token_count + self.provider.max_tokens + 32 <= context_limit:
                    break
                if len(bounded) <= 2:
                    raise InferenceError("Companion world context is too large for the configured model window; reduce host context or increase its context setting.")
                # Preserve authoritative host state and the latest instruction.
                # Discard only oldest dialogue, never facts or current commands.
                bounded.pop(1)
            if not prompt.endswith("<|start|>assistant to=user<|message|> "):
                raise InferenceError("Muse direct speech requires its native to=user chat template")
            body = {**extra, "prompt": prompt, "json_schema": reply_schema_for_messages(messages),
                "temperature": self.provider.temperature, "n_predict": self.provider.max_tokens,
                "stream": True, "cache_prompt": True, "stop": ["<|eot|>", "<|eom|>"]}
            async with client.stream("POST", "completion", json=body) as response:
                response.raise_for_status()
                async for line in response.aiter_lines():
                    if cancel.is_set():
                        break
                    if not line.startswith("data:"):
                        continue
                    payload = line[5:].strip()
                    if payload == "[DONE]":
                        break
                    event = json.loads(payload)
                    if event.get("error"):
                        raise InferenceError(str(event["error"]))
                    yield InferenceChunk(delta=event.get("content", ""), done=bool(event.get("stop")))

    async def close(self) -> None:
        await self.client.close()


class InferenceHub:
    def __init__(self, cfg: AppConfig):
        self.cfg = cfg
        self._process: asyncio.subprocess.Process | None = None
        self._discover_cache: dict[str, Discovery] = {}
        self._start_lock = asyncio.Lock()
        self._process_log = None

    def get_backend(self, provider: InferenceProvider) -> InferenceBackend:
        if provider.kind == InferenceProviderKind.mock:
            return MockBackend(provider)
        return OpenAICompatBackend(provider)

    async def resolve(self, preferred: InferenceProvider | None, model_override: str | None) -> InferenceProvider:
        candidates: list[InferenceProvider] = []
        if preferred:
            candidates.append(preferred)
        if not preferred:
            candidates.extend(p for p in self.cfg.providers if p.enabled and p.kind != InferenceProviderKind.mock)

        last_error = "No local model is available. Start Ollama or configure llama.cpp in Setup. Select Demo explicitly to test without a model."
        for provider in candidates:
            provider = provider.model_copy()
            if not provider.enabled:
                raise InferenceError(f"Provider {provider.name} is disabled")
            if model_override:
                provider.model = model_override
            if provider.kind == InferenceProviderKind.mock:
                return provider
            if provider.kind == InferenceProviderKind.llama_cpp:
                try:
                    await self.ensure_llama_server(provider)
                except InferenceError as exc:
                    last_error = str(exc)
                    continue
            discovery = await self.discover(provider)
            if not discovery.reachable:
                last_error = discovery.error or last_error
                continue
            if provider.model and discovery.models and provider.model not in discovery.models:
                last_error = f"Model {provider.model} is not available in {provider.name}. Choose an installed model in Setup."
                continue
            if not provider.model:
                if discovery.models:
                    provider.model = discovery.models[0]
                else:
                    last_error = f"{provider.name} is reachable but lists no models"
                    continue
            return provider
        raise InferenceError(last_error)

    async def discover(self, provider: InferenceProvider, force: bool = False) -> Discovery:
        if provider.kind == InferenceProviderKind.mock:
            return Discovery(reachable=True, models=[provider.model or "mock-character"], latency_ms=0)
        if not provider.base_url:
            return Discovery(reachable=False, error="No base URL")
        url = provider.base_url.rstrip("/") + "/models"
        started = time.perf_counter()
        try:
            async with httpx.AsyncClient(timeout=3.0) as client:
                response = await client.get(url, headers={"Authorization": f"Bearer {provider.api_key}"})
                response.raise_for_status()
                payload = response.json()
            models = [item.get("id") for item in payload.get("data", []) if item.get("id")]
            if provider.kind == InferenceProviderKind.ollama and not models:
                models = await _ollama_tags(provider)
            discovery = Discovery(
                reachable=True,
                models=models,
                latency_ms=(time.perf_counter() - started) * 1000,
            )
        except Exception as exc:
            discovery = Discovery(reachable=False, error=str(exc))
        self._discover_cache[provider.id] = discovery
        return discovery

    async def probe_all(self) -> list[dict[str, Any]]:
        async def probe(provider):
            discovery = await self.discover(provider, force=True)
            return {"provider": provider.model_dump(), "reachable": discovery.reachable,
                    "models": discovery.models, "error": discovery.error,
                    "latency_ms": discovery.latency_ms}
        return await asyncio.gather(*(probe(p) for p in self.cfg.providers if p.enabled))

    async def ensure_llama_server(self, provider: InferenceProvider) -> None:
        async with self._start_lock:
            await self._ensure_llama_server(provider)

    async def _ensure_llama_server(self, provider: InferenceProvider) -> None:
        model = _resolve_gguf(self.cfg.llama_model_path)
        discovery = await self.discover(provider, force=True)
        if discovery.reachable and model and _server_has_model(discovery.models, model):
            return
        if discovery.reachable and model:
            log.info("llama-server is running a different model; restarting for %s", model.name)
            await self.stop_llama_server()
        binary = _resolve_llama_server(self.cfg.llama_server_path, model)
        if not binary:
            raise InferenceError(
                "llama.cpp server is not running and no llama-server executable was found. "
                "Install Ollama, start llama-server, or set llama_server_path in settings."
            )
        if not model:
            raise InferenceError(
                "llama.cpp server needs a GGUF model. Set llama_model_path or place a .gguf file "
                f"in {models_dir()}."
            )
        await self.start_llama_server(binary, model, provider)

    async def stop_llama_server(self) -> None:
        if self._process and self._process.returncode is None:
            self._process.terminate()
            try:
                await asyncio.wait_for(self._process.wait(), timeout=8)
            except TimeoutError:
                self._process.kill()
        self._process = None
        if self._process_log:
            self._process_log.close()
            self._process_log = None

    async def start_llama_server(self, binary: Path, model: Path, provider: InferenceProvider) -> None:
        await self.stop_llama_server()
        # Replacing a configured model is explicit; ordinary shutdown must never
        # kill another runtime's shared server merely because it uses this port.
        _free_port(self.cfg.llama_port)
        port = self.cfg.llama_port
        args = llama_server_args(self.cfg, binary, model)
        log.info("Starting llama-server: %s", " ".join(args))
        from localtalker.paths import user_data_dir
        self._process_log = (user_data_dir() / "llama-server.log").open("ab")
        self._process = await asyncio.create_subprocess_exec(
            *args, stdout=self._process_log, stderr=self._process_log,
            cwd=str(binary.parent),
            **({"creationflags": 0x08000000} if os.name == "nt" else {}),
        )
        provider.base_url = f"http://127.0.0.1:{port}/v1"
        deadline = time.time() + (240 if "glimmer" in model.name.lower() else 90)
        last_err = "llama-server did not become ready"
        while time.time() < deadline:
            if self._process.returncode is not None:
                stderr = ""
                if self._process.stderr:
                    stderr = (await self._process.stderr.read()).decode("utf-8", errors="replace")[-2000:]
                raise InferenceError(f"llama-server exited early (code {self._process.returncode}). Check llama-server.log in the data directory and confirm CUDA libraries and VRAM are available.")
            discovery = await self.discover(provider, force=True)
            if discovery.reachable:
                log.info("llama-server ready with models %s", discovery.models)
                return
            last_err = discovery.error or last_err
            await asyncio.sleep(0.4)
        raise InferenceError(last_err)

    async def close(self) -> None:
        await self.stop_llama_server()


def llama_server_args(cfg: AppConfig, binary: Path, model: Path) -> list[str]:
    args = [
        str(binary),
        "-m",
        str(model),
        "--host",
        "127.0.0.1",
        "--port",
        str(cfg.llama_port),
        "-c",
        str(cfg.llama_ctx),
        "-ngl",
        str(cfg.llama_n_gpu_layers),
    ]
    if cfg.llama_flash_attn:
        args.extend(["-fa", "on"])
    if cfg.llama_jinja:
        args.append("--jinja")
    if "glimmer" in model.name.lower():
        args.extend(
            [
                "--alias",
                "muse-glimmer",
                "--chat-template-kwargs",
                '{"reasoning_strength":"low"}',
                "--reasoning-budget",
                "128",
            ]
        )
    return args


def _server_has_model(listed: list[str], model: Path) -> bool:
    names = {Path(item).name.lower() for item in listed}
    aliases = {item.lower() for item in listed}
    if model.name.lower() in names or str(model).lower() in aliases:
        return True
    return "glimmer" in model.name.lower() and "muse-glimmer" in aliases


def _free_port(port: int) -> None:
    if os.name != "nt":
        return
    try:
        import subprocess

        result = subprocess.run(
            ["netstat", "-ano"],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
        pids: set[str] = set()
        for line in result.stdout.splitlines():
            if f":{port}" in line and "LISTENING" in line:
                pids.add(line.split()[-1])
        for pid in pids:
            if pid.isdigit() and pid != "0":
                subprocess.run(["taskkill", "/PID", pid, "/F"], capture_output=True, timeout=5, check=False)
    except Exception as exc:
        log.warning("Could not free port %s: %s", port, exc)


def _chunk_text(text: str, size: int) -> list[str]:
    return [text[i : i + size] for i in range(0, len(text), size)]


def _json_escape(text: str) -> str:
    return (
        text.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\n", "\\n")
        .replace("\r", "")
    )


async def _ollama_tags(provider: InferenceProvider) -> list[str]:
    root = provider.base_url.replace("/v1", "")
    try:
        async with httpx.AsyncClient(timeout=3.0) as client:
            response = await client.get(root.rstrip("/") + "/api/tags")
            response.raise_for_status()
            payload = response.json()
        return [item.get("name") for item in payload.get("models", []) if item.get("name")]
    except Exception:
        return []


MIN_GLIMMER_LLAMA_BUILD = 10353


def _llama_build_number(path: Path) -> int:
    for part in path.parts:
        label = part.lower()
        if label.startswith("llama-b") and label[7:].isdigit():
            return int(label[7:])
    return 0


def _llama_server_candidates(configured: str) -> list[Path]:
    found: list[Path] = []
    seen: set[Path] = set()

    def add(path: Path | None) -> None:
        if not path or not path.is_file():
            return
        resolved = path.resolve()
        if resolved in seen:
            return
        seen.add(resolved)
        found.append(resolved)

    add(Path(configured) if configured else None)
    for name in ("llama-server.exe", "llama-server"):
        located = shutil.which(name)
        add(Path(located) if located else None)
    roots = [runtimes_dir(), *[folder / "runtimes" for folder in extra_data_dirs()]]
    for root in roots:
        if not root.is_dir():
            continue
        add(root / "llama-server.exe")
        add(root / "llama-server")
        for path in root.glob("*/llama-server.exe"):
            add(path)
        for path in root.glob("*/llama-server"):
            add(path)
    return found


def _resolve_llama_server(configured: str, model: Path | None = None) -> Path | None:
    candidates = _llama_server_candidates(configured)
    if not candidates:
        return None
    if model and "glimmer" in model.name.lower():
        supported = [path for path in candidates if _llama_build_number(path) >= MIN_GLIMMER_LLAMA_BUILD]
        if supported:
            return max(supported, key=_llama_build_number)
    if configured and Path(configured).is_file():
        return Path(configured)
    return candidates[0]


def _resolve_gguf(configured: str) -> Path | None:
    if configured and Path(configured).is_file():
        return Path(configured)
    extra = [models_dir()]
    for folder in extra:
        if not folder.is_dir():
            continue
        files = sorted(folder.glob("*.gguf"), key=lambda p: p.stat().st_size)
        if files:
            return files[0]
    env = os.environ.get("LOCALTALKER_GGUF")
    if env and Path(env).is_file():
        return Path(env)
    return None


def gpu_info() -> dict[str, Any]:
    info: dict[str, Any] = {"available": False}
    try:
        import subprocess

        result = subprocess.run(
            ["nvidia-smi", "--query-gpu=name,memory.total,memory.used,driver_version", "--format=csv,noheader"],
            capture_output=True,
            text=True,
            timeout=4,
            check=False,
        )
        if result.returncode == 0 and result.stdout.strip():
            name, total, used, driver = [part.strip() for part in result.stdout.strip().splitlines()[0].split(",")]
            info = {
                "available": True,
                "name": name,
                "memory_total": total,
                "memory_used": used,
                "driver": driver,
            }
    except Exception as exc:
        info["error"] = str(exc)
    return info
