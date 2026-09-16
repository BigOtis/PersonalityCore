import asyncio
import json

import pytest
from starlette.testclient import TestClient

from localtalker.api import build_runtime, create_app
from localtalker.inference import InferenceChunk
from tests.conftest import FakeSpeech


class PausingBackend:
    closed = False

    async def stream(self, messages, cancel):
        yield InferenceChunk('{"dialogue":"Still thinking')
        await asyncio.sleep(30)
        yield InferenceChunk('"}')

    async def close(self):
        self.closed = True


def receive_until(ws, kind):
    for _ in range(50):
        event = ws.receive_json()
        if event["type"] == kind:
            return event
    raise AssertionError(f"Missing {kind}")


def test_interrupt_and_ping_work_during_blocked_inference(tmp_path, mock_config):
    runtime = build_runtime(mock_config, tmp_path / "live.db")
    runtime.speech = FakeSpeech()
    backend = PausingBackend()
    runtime.inference.get_backend = lambda _: backend
    with TestClient(create_app(runtime)) as client:
        char = client.get("/v1/characters").json()[0]
        sid = client.post("/v1/sessions", json={"character_id": char["id"]}).json()["id"]
        with client.websocket_connect(f"/v1/sessions/{sid}/live") as ws:
            receive_until(ws, "ready")
            ws.send_json({"type": "text", "text": "Start"})
            receive_until(ws, "token")
            ws.send_json({"type": "ping"})
            receive_until(ws, "pong")
            ws.send_json({"type": "interrupt"})
            receive_until(ws, "cancelled")
            ws.send_json({"type": "ping"})
            receive_until(ws, "pong")
            assert backend.closed
            history = client.get(f"/v1/sessions/{sid}").json()["messages"]
            assert [m["role"] for m in history] == ["user"]
            assert runtime.speech.received_text == []
            ws.send_json(["invalid"])
            assert receive_until(ws, "error")["error"].startswith("Invalid message")
            ws.send_json({"type": "ping"})
            receive_until(ws, "pong")


async def test_parallel_characters_and_scoped_cancellation(tmp_path, mock_config):
    runtime = build_runtime(mock_config, tmp_path / "parallel.db")
    runtime.speech = FakeSpeech()
    await runtime.store.connect()
    chars = await runtime.store.list_characters()
    first = await runtime.ensure_session(chars[0].id)
    second = await runtime.ensure_session(chars[1].id)
    backend = PausingBackend()
    original = runtime.inference.get_backend
    runtime.inference.get_backend = lambda _: backend
    token = asyncio.Event()

    async def emit(event):
        if event.type == "token":
            token.set()

    task = asyncio.create_task(runtime.run_turn(first.id, "Wait", emit))
    try:
        await asyncio.wait_for(token.wait(), 2)
        runtime.inference.get_backend = original
        reply = await asyncio.wait_for(runtime.run_turn(second.id, "Other character", emit), 2)
        assert "Other character" in reply.dialogue
        assert not task.done(), "Another character's turn must not cancel this one"
        await runtime.interrupt(session_id=first.id)
        await asyncio.wait_for(task, 2)
        assert backend.closed
        assert len((await runtime.store.get_session(second.id)).messages) == 2
    finally:
        await runtime.shutdown()


def test_untrusted_websocket_origin_rejected(tmp_path, mock_config):
    from starlette.websockets import WebSocketDisconnect
    runtime = build_runtime(mock_config, tmp_path / "origin.db")
    runtime.speech = FakeSpeech()
    with TestClient(create_app(runtime)) as client:
        char = client.get("/v1/characters").json()[0]
        sid = client.post("/v1/sessions", json={"character_id": char["id"]}).json()["id"]
        with pytest.raises(WebSocketDisconnect) as exc:
            with client.websocket_connect(f"/v1/sessions/{sid}/live", headers={"origin": "https://untrusted.example"}):
                pass
        assert exc.value.code == 4403
