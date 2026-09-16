"""Hardware-dependent smoke tests.

These talk to a real local OpenAI-compatible server (Ollama, llama.cpp, LM Studio).
They are skipped unless LOCALTALKER_SMOKE=1 and a provider is reachable.
"""

from __future__ import annotations

import os

import pytest

from localtalker.config import load_config
from localtalker.inference import InferenceHub
from localtalker.models import InferenceProviderKind

pytestmark = pytest.mark.smoke


def _enabled() -> bool:
    return os.environ.get("LOCALTALKER_SMOKE") == "1"


@pytest.mark.skipif(not _enabled(), reason="Set LOCALTALKER_SMOKE=1 to run real inference smoke tests")
async def test_real_provider_streams_tokens():
    cfg = load_config()
    hub = InferenceHub(cfg)
    live = None
    for provider in cfg.providers:
        if provider.kind == InferenceProviderKind.mock:
            continue
        discovery = await hub.discover(provider, force=True)
        if discovery.reachable and (provider.model or discovery.models):
            live = provider.model_copy()
            if not live.model:
                live.model = discovery.models[0]
            break
    if live is None:
        pytest.skip("No reachable local inference provider")

    backend = hub.get_backend(live)
    cancel = __import__("asyncio").Event()
    chunks = []
    async for chunk in backend.stream(
        [
            {"role": "system", "content": 'Reply with JSON: {"dialogue":"..."}'},
            {"role": "user", "content": "Say hello in one short sentence."},
        ],
        cancel,
    ):
        if chunk.delta:
            chunks.append(chunk.delta)
    await backend.close()
    text = "".join(chunks)
    assert text.strip(), f"{live.name} produced an empty response"
