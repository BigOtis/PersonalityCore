import json

import pytest
from httpx import ASGITransport, AsyncClient

from localtalker.api import create_app, build_runtime
from localtalker.models import CharacterCreate
from tests.conftest import FakeSpeech


@pytest.fixture
async def client(tmp_path, mock_config):
    runtime = build_runtime(mock_config, tmp_path / "db.sqlite3")
    runtime.speech = FakeSpeech()
    await runtime.startup()
    app = create_app(runtime, start=False)
    transport = ASGITransport(app=app)
    try:
        async with AsyncClient(transport=transport, base_url="http://test") as http:
            yield http, runtime
    finally:
        await runtime.shutdown()


async def test_health_and_character_protocol(client):
    http, _runtime = client
    health = await http.get("/v1/health")
    assert health.status_code == 200
    assert health.json()["ok"] is True
    characters = (await http.get("/v1/characters")).json()
    assert len(characters) >= 3
    created = await http.post("/v1/characters", json={"name": "Nox", "personality": "Quiet shade."})
    assert created.status_code == 201
    assert created.json()["name"] == "Nox"


async def test_turn_endpoint_streams_through_events(client):
    http, runtime = client
    characters = (await http.get("/v1/characters")).json()
    session = (await http.post("/v1/sessions", json={"character_id": characters[0]["id"]})).json()
    turn = await http.post(f"/v1/sessions/{session['id']}/turns", json={"text": "Good evening."})
    assert turn.status_code == 200
    body = turn.json()
    assert body["reply"]["dialogue"]
    types = [event["type"] for event in body["events"]]
    assert "token" in types
    assert "reply" in types
    assert "audio" in types
    persisted = (await http.get(f"/v1/sessions/{session['id']}")).json()
    assert persisted["messages"][0]["content"] == "Good evening."
    assert persisted["messages"][1]["reply"]["dialogue"]
    assert runtime.speech.received_text


async def test_context_injection_roundtrip(client):
    http, _runtime = client
    character_id = (await http.get("/v1/characters")).json()[0]["id"]
    session = (await http.post("/v1/sessions", json={"character_id": character_id})).json()
    patched = await http.post(
        f"/v1/sessions/{session['id']}/context",
        json={"game_context": {"location": "clocktower", "inventory": ["lens"]}},
    )
    assert patched.status_code == 200
    assert patched.json()["game_context"]["location"] == "clocktower"
    loaded = (await http.get(f"/v1/sessions/{session['id']}")).json()
    assert loaded["game_context"]["inventory"] == ["lens"]


async def test_unknown_session_is_an_error(client):
    http, _runtime = client
    response = await http.post("/v1/sessions/sess_missing/turns", json={"text": "hi"})
    assert response.status_code == 404


async def test_editing_voice_roundtrips_a_valid_character(client):
    http, runtime = client
    cid = (await http.get("/v1/characters")).json()[0]["id"]
    edited = await http.patch(f"/v1/characters/{cid}", json={"voice": {"voice_id": "am_michael", "speed": 0.9}})
    assert edited.status_code == 200
    character = await runtime.store.get_character(cid)
    assert character.voice.voice_id == "am_michael"
    assert character.voice.speed == 0.9


async def test_invalid_configuration_and_character_are_recoverable(client, monkeypatch, tmp_path):
    http, _ = client
    monkeypatch.setattr("localtalker.config.config_path", lambda: tmp_path / "config.json")
    assert (await http.patch("/v1/config", json={"port": -1})).status_code == 422
    assert (await http.post("/v1/characters", json={"name": "   "})).status_code == 422
    assert (await http.post("/v1/voices/preview", json={"speed": 0})).status_code == 422
    changed = await http.patch("/v1/config", json={"speech": {"tts_enabled": False}})
    assert changed.status_code == 200
    assert changed.json()["speech"]["whisper_model"] == "base.en"


async def test_text_mode_does_not_synthesize_when_disabled(client):
    http, runtime = client
    runtime.cfg.speech.tts_enabled = False
    cid = (await http.get("/v1/characters")).json()[0]["id"]
    sid = (await http.post("/v1/sessions", json={"character_id": cid})).json()["id"]
    result = (await http.post(f"/v1/sessions/{sid}/turns", json={"text": "Text only"})).json()
    assert result["reply"]["dialogue"]
    assert not any(e["type"] == "audio" for e in result["events"])
    assert runtime.speech.received_text == []
