"""End-to-end application flow against the real runtime HTTP/WebSocket surface.

Uses the mock inference provider and a lightweight speech double so the test
proves the product path without requiring a GPU model.
"""

from __future__ import annotations

import json

import pytest
from httpx import ASGITransport, AsyncClient
from starlette.testclient import TestClient

from localtalker.api import build_runtime, create_app
from tests.conftest import FakeSpeech


def _app(tmp_path, mock_config):
    runtime = build_runtime(mock_config, tmp_path / "e2e.sqlite3")
    runtime.speech = FakeSpeech()
    return create_app(runtime, start=True), runtime


@pytest.mark.e2e
async def test_application_flow_text_to_voice_and_history(tmp_path, mock_config):
    runtime = build_runtime(mock_config, tmp_path / "e2e.sqlite3")
    runtime.speech = FakeSpeech()
    await runtime.startup()
    app = create_app(runtime, start=False)
    transport = ASGITransport(app=app)
    async with AsyncClient(transport=transport, base_url="http://localtalker") as http:
        assert (await http.get("/v1/health")).json()["ok"] is True
        status = (await http.get("/v1/status")).json()
        assert status["ready"] is True

        characters = (await http.get("/v1/characters")).json()
        assert characters, "seed characters should exist"
        character = characters[0]
        session = (
            await http.post(
                "/v1/sessions",
                json={"character_id": character["id"], "game_context": {"place": "studio"}},
            )
        ).json()

        turn = await http.post(
            f"/v1/sessions/{session['id']}/turns",
            json={"text": "Tell me who you are."},
        )
        assert turn.status_code == 200
        payload = turn.json()
        assert payload["reply"]["dialogue"]
        event_types = [event["type"] for event in payload["events"]]
        assert event_types[0] in {"user_message", "state"}
        assert "token" in event_types
        assert "reply" in event_types
        assert any(event["type"] == "audio" and event.get("pcm16_b64") for event in payload["events"])
        assert runtime.speech.received_text, "speech pipeline must receive spoken text"

        history = (await http.get(f"/v1/sessions/{session['id']}")).json()
        assert len(history["messages"]) == 2
        assert history["messages"][0]["role"] == "user"
        assert history["messages"][1]["role"] == "assistant"
        assert history["game_context"]["place"] == "studio"
    await runtime.shutdown()


@pytest.mark.e2e
def test_websocket_live_protocol(tmp_path, mock_config):
    app, runtime = _app(tmp_path, mock_config)
    with TestClient(app) as client:
        character = client.get("/v1/characters").json()[0]
        session = client.post("/v1/sessions", json={"character_id": character["id"]}).json()
        with client.websocket_connect(f"/v1/sessions/{session['id']}/live") as ws:
            ready = json.loads(ws.receive_text())
            assert ready["type"] == "ready"
            ws.send_text(json.dumps({"type": "text", "text": "Can you hear me?"}))
            types = []
            reply = None
            for _ in range(40):
                message = json.loads(ws.receive_text())
                types.append(message["type"])
                if message["type"] == "reply":
                    reply = message
                    break
            assert "token" in types
            assert reply is not None
            assert reply["reply"]["dialogue"]
            assert runtime.speech.received_text
