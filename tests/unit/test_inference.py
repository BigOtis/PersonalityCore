import pytest
from localtalker.config import AppConfig, default_providers
from localtalker.inference import Discovery, InferenceError, InferenceHub


async def test_automatic_resolution_never_silently_returns_mock():
    hub = InferenceHub(AppConfig(providers=default_providers()))
    async def offline(*args, **kwargs):
        return Discovery(False, error="offline")
    async def no_llama(*args):
        raise InferenceError("llama.cpp not configured")
    hub.discover = offline
    hub.ensure_llama_server = no_llama
    with pytest.raises(InferenceError):
        await hub.resolve(None, None)
    demo = await hub.resolve(hub.cfg.providers[0], None)
    assert demo.kind == "mock"


async def test_explicit_model_is_not_substituted():
    hub = InferenceHub(AppConfig(providers=default_providers()))
    async def online(*args, **kwargs):
        return Discovery(True, models=["installed"])
    hub.discover = online
    with pytest.raises(InferenceError, match="not available"):
        await hub.resolve(hub.cfg.providers[1], "missing")


async def test_closing_unowned_hub_does_not_kill_shared_model(monkeypatch):
    import localtalker.inference as inference
    killed = []
    monkeypatch.setattr(inference, "_free_port", lambda port: killed.append(port))
    hub = InferenceHub(AppConfig())
    await hub.close()
    assert killed == []


@pytest.mark.parametrize("model,kind,enabled", [
    ("muse-glimmer", "llama_cpp", True),
    ("another-model", "llama_cpp", False),
    ("muse-glimmer", "openai_compat", False),
])
async def test_direct_speech_prefill_is_scoped_and_preserves_input(model, kind, enabled):
    import asyncio
    from types import SimpleNamespace
    from localtalker.models import InferenceProvider
    from localtalker.inference import OpenAICompatBackend
    provider = InferenceProvider(id="test", name="Test", kind=kind, model=model,
        base_url="http://localhost:8090/v1", extra={"direct_speech": True})
    backend = OpenAICompatBackend(provider)
    calls = []
    direct_calls = []
    async def direct(messages, cancel, extra):
        from localtalker.inference import InferenceChunk
        direct_calls.append((messages, extra))
        yield InferenceChunk(delta="", done=True)
    backend._stream_direct = direct
    class Stream:
        def __aiter__(self): return self
        async def __anext__(self): raise StopAsyncIteration
        async def close(self): pass
    async def create(**kwargs):
        calls.append(kwargs)
        return Stream()
    await backend.client.close()
    backend.client = SimpleNamespace(chat=SimpleNamespace(completions=SimpleNamespace(create=create)))
    messages = [{"role": "user", "content": "Hello"}]
    _ = [chunk async for chunk in backend.stream(messages, asyncio.Event())]
    assert messages == [{"role": "user", "content": "Hello"}]
    assert bool(direct_calls) == enabled
    assert bool(calls) != enabled
    extra = direct_calls[0][1] if enabled else calls[0].get("extra_body", {})
    assert "direct_speech" not in extra


async def test_native_muse_completion_enforces_schema(monkeypatch):
    import asyncio
    import json
    import httpx
    from localtalker.models import InferenceProvider
    from localtalker.inference import OpenAICompatBackend
    from localtalker.prompts import REPLY_SCHEMA
    provider = InferenceProvider(id="test", name="Test", kind="llama_cpp", model="muse-glimmer",
        base_url="http://localhost:8090/v1", extra={"direct_speech": True})
    backend = OpenAICompatBackend(provider)
    requests = []
    def respond(request):
        body = json.loads(request.content)
        requests.append((request.url.path, body))
        if request.url.path == "/apply-template":
            return httpx.Response(200, json={"prompt": "context<|start|>assistant to=user<|message|> "})
        return httpx.Response(200, text='data: {"content":"{\\"dialogue\\":\\"Hello\\"}","stop":false}\n\ndata: {"content":"","stop":true}\n\n')
    client_class = httpx.AsyncClient
    monkeypatch.setattr(httpx, "AsyncClient", lambda **kwargs: client_class(**kwargs, transport=httpx.MockTransport(respond)))
    try:
        chunks = [c async for c in backend.stream([{"role": "user", "content": "Hi"}], asyncio.Event())]
    finally:
        await backend.close()
    assert "".join(c.delta for c in chunks) == '{"dialogue":"Hello"}'
    assert chunks[-1].done
    assert requests[0][1]["messages"][-1]["content"] == " "
    assert requests[1][0] == "/completion"
    assert requests[1][1]["json_schema"] == REPLY_SCHEMA

async def test_companion_budget_drops_old_dialogue_preserves_current_instruction(monkeypatch):
    import asyncio,json,httpx
    from localtalker.models import InferenceProvider
    from localtalker.inference import OpenAICompatBackend
    provider=InferenceProvider(id='budget',name='Budget',kind='llama_cpp',model='muse-glimmer',base_url='http://localhost:8090/v1',max_tokens=300,extra={'direct_speech':True})
    backend=OpenAICompatBackend(provider)
    rendered=[]
    def respond(request):
        path=request.url.path
        if path=='/props':return httpx.Response(200,json={'default_generation_settings':{'n_ctx':1000}})
        body=json.loads(request.content)
        if path=='/apply-template':
            rendered.append(body['messages'])
            return httpx.Response(200,json={'prompt':'context<|start|>assistant to=user<|message|> '})
        if path=='/tokenize':return httpx.Response(200,json={'tokens':[1]*(800 if len(rendered)==1 else 500)})
        return httpx.Response(200,text='data: {"content":"{}","stop":true}\n\n')
    client_class=httpx.AsyncClient
    monkeypatch.setattr(httpx,'AsyncClient',lambda **kw:client_class(**kw,transport=httpx.MockTransport(respond)))
    messages=[{'role':'system','content':'COMMAND COMPANION: host facts'},{'role':'user','content':'old command'},{'role':'user','content':'Print now'}]
    try:
        _=[c async for c in backend.stream(messages,asyncio.Event())]
    finally:await backend.close()
    assert len(rendered)==2
    assert rendered[-1][0]==messages[0] and rendered[-1][-2]==messages[-1]
    assert len(messages)==3
