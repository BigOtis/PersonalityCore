import asyncio

from localtalker.api import build_runtime
from localtalker.models import CharacterReply, LiveServerMessage
from tests.conftest import FakeSpeech


async def _collect_turn(runtime, session_id: str, text: str):
    events: list[LiveServerMessage] = []

    async def emit(message: LiveServerMessage) -> None:
        events.append(message)

    reply = await runtime.run_turn(session_id, text, emit)
    return reply, events


async def test_text_turn_streams_and_persists(tmp_path, mock_config):
    runtime = build_runtime(mock_config, tmp_path / "db.sqlite3")
    runtime.speech = FakeSpeech()
    await runtime.store.connect()
    characters = await runtime.store.list_characters()
    session = await runtime.ensure_session(characters[0].id)
    reply, events = await _collect_turn(runtime, session.id, "Any rooms left?")
    assert isinstance(reply, CharacterReply)
    assert "Any rooms left?" in reply.dialogue
    assert reply.intent == "acknowledge"
    types = [event.type for event in events]
    assert "token" in types
    assert "reply" in types
    assert "audio" in types
    assert "state" in types
    loaded = await runtime.store.get_session(session.id)
    assert loaded is not None
    assert loaded.messages[0].content == "Any rooms left?"
    assert loaded.messages[1].reply is not None
    assert loaded.messages[1].reply.dialogue
    assert runtime.speech.received_text
    await runtime.store.close()


async def test_cancellation_stops_generation(tmp_path, mock_config):
    runtime = build_runtime(mock_config, tmp_path / "db.sqlite3")
    runtime.speech = FakeSpeech()
    await runtime.store.connect()
    session = await runtime.ensure_session((await runtime.store.list_characters())[0].id)
    events: list[str] = []

    async def emit(message: LiveServerMessage) -> None:
        events.append(message.type)
        if message.type == "token":
            await runtime.interrupt()

    await runtime.run_turn(session.id, "Interrupt me", emit)
    assert "cancelled" in events or events[-1] in {"state", "cancelled"}
    await runtime.store.close()


async def test_game_context_survives_turn(tmp_path, mock_config):
    runtime = build_runtime(mock_config, tmp_path / "db.sqlite3")
    runtime.speech = FakeSpeech()
    await runtime.store.connect()
    session = await runtime.ensure_session((await runtime.store.list_characters())[0].id)
    session.game_context = {"location": "docks", "nearby": ["ferry"]}
    await runtime.store.save_session(session)
    await _collect_turn(runtime, session.id, "Where am I?")
    loaded = await runtime.store.get_session(session.id)
    assert loaded.game_context["location"] == "docks"
    await runtime.store.close()
