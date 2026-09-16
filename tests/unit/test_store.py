from pathlib import Path

import pytest

from localtalker.models import CharacterCreate, CharacterUpdate, ChatMessage, ChatRole, SessionCreate
from localtalker.store import Store


@pytest.fixture
async def store(tmp_path: Path) -> Store:
    st = Store(tmp_path / "test.sqlite3")
    await st.connect()
    yield st
    await st.close()


async def test_seed_characters_and_roundtrip(store: Store):
    characters = await store.list_characters()
    assert len(characters) >= 3
    names = {c.name for c in characters}
    assert {"Mira", "Rook", "Ivy"} <= names
    mira = next(c for c in characters if c.name == "Mira")
    updated = await store.update_character(mira.id, CharacterUpdate(personality="Even warmer."))
    assert updated is not None
    assert updated.personality == "Even warmer."
    loaded = await store.get_character(mira.id)
    assert loaded is not None
    assert loaded.personality == "Even warmer."


async def test_character_temperature_is_optional_and_persists(store: Store):
    default = await store.create_character(CharacterCreate(name="Bly"))
    assert default.temperature is None  # Falls back to the provider's own setting.
    colder = await store.create_character(CharacterCreate(name="Guide", temperature=0.45))
    assert (await store.get_character(colder.id)).temperature == 0.45
    warmed = await store.update_character(colder.id, CharacterUpdate(temperature=1.0))
    assert warmed is not None and warmed.temperature == 1.0


async def test_session_messages_persist(store: Store):
    character = await store.create_character(CharacterCreate(name="Ash"))
    session = await store.create_session(SessionCreate(character_id=character.id, game_context={"hour": 3}))
    session = await store.append_message(session, ChatMessage(role=ChatRole.user, content="Hello"))
    loaded = await store.get_session(session.id)
    assert loaded is not None
    assert loaded.game_context["hour"] == 3
    assert loaded.messages[0].content == "Hello"
    assert await store.delete_character(character.id)
    assert await store.get_character(character.id) is None
    assert await store.get_session(session.id) is None
