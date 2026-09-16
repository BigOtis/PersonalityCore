from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path

import aiosqlite

from localtalker.models import (
    Character,
    CharacterCreate,
    CharacterUpdate,
    ChatMessage,
    Session,
    SessionCreate,
    utcnow,
)
from localtalker.prompts import seed_characters


class Store:
    def __init__(self, path: Path):
        self.path = path
        self._db: aiosqlite.Connection | None = None

    async def connect(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._db = await aiosqlite.connect(self.path)
        self._db.row_factory = aiosqlite.Row
        await self._db.execute("PRAGMA journal_mode=WAL")
        await self._db.execute("PRAGMA foreign_keys=ON")
        await self._create()
        await self._seed()

    async def close(self) -> None:
        if self._db:
            await self._db.close()
            self._db = None

    @property
    def db(self) -> aiosqlite.Connection:
        if self._db is None:
            raise RuntimeError("Store is not connected")
        return self._db

    async def _create(self) -> None:
        await self.db.executescript(
            """
            CREATE TABLE IF NOT EXISTS characters (
                id TEXT PRIMARY KEY,
                data TEXT NOT NULL
            );
            CREATE TABLE IF NOT EXISTS sessions (
                id TEXT PRIMARY KEY,
                character_id TEXT NOT NULL,
                data TEXT NOT NULL
            );
            """
        )
        await self.db.commit()

    async def _seed(self) -> None:
        existing = await self.list_characters()
        if existing:
            return
        for item in seed_characters():
            await self.create_character(CharacterCreate.model_validate(item))

    async def list_characters(self) -> list[Character]:
        cur = await self.db.execute("SELECT data FROM characters")
        rows = await cur.fetchall()
        chars = [Character.model_validate_json(row["data"]) for row in rows]
        chars.sort(key=lambda c: c.created_at)
        return chars

    async def get_character(self, character_id: str) -> Character | None:
        cur = await self.db.execute("SELECT data FROM characters WHERE id = ?", (character_id,))
        row = await cur.fetchone()
        return Character.model_validate_json(row["data"]) if row else None

    async def create_character(self, payload: CharacterCreate) -> Character:
        colors = ["#e2a35a", "#7eb8b0", "#d4785a", "#c4a574", "#8f9d6a"]
        count = len(await self.list_characters())
        character = Character(
            name=payload.name.strip(),
            personality=payload.personality,
            instructions=payload.instructions,
            provider_id=payload.provider_id,
            model=payload.model,
            temperature=payload.temperature,
            voice=payload.voice,
            color=payload.color or colors[count % len(colors)],
        )
        await self.db.execute(
            "INSERT INTO characters (id, data) VALUES (?, ?)",
            (character.id, character.model_dump_json()),
        )
        await self.db.commit()
        return character

    async def update_character(self, character_id: str, payload: CharacterUpdate) -> Character | None:
        character = await self.get_character(character_id)
        if not character:
            return None
        data = payload.model_dump(exclude_unset=True)
        updated = Character.model_validate({**character.model_dump(), **data})
        updated.updated_at = utcnow()
        await self.db.execute(
            "UPDATE characters SET data = ? WHERE id = ?",
            (updated.model_dump_json(), character_id),
        )
        await self.db.commit()
        return updated

    async def delete_character(self, character_id: str) -> bool:
        await self.db.execute("DELETE FROM sessions WHERE character_id = ?", (character_id,))
        cur = await self.db.execute("DELETE FROM characters WHERE id = ?", (character_id,))
        await self.db.commit()
        return cur.rowcount > 0

    async def list_sessions(self, character_id: str | None = None) -> list[Session]:
        if character_id:
            cur = await self.db.execute(
                "SELECT data FROM sessions WHERE character_id = ?", (character_id,)
            )
        else:
            cur = await self.db.execute("SELECT data FROM sessions")
        rows = await cur.fetchall()
        sessions = [Session.model_validate_json(row["data"]) for row in rows]
        sessions.sort(key=lambda s: s.updated_at, reverse=True)
        return sessions

    async def get_session(self, session_id: str) -> Session | None:
        if self._db is None:
            return None
        cur = await self.db.execute("SELECT data FROM sessions WHERE id = ?", (session_id,))
        row = await cur.fetchone()
        return Session.model_validate_json(row["data"]) if row else None

    async def create_session(self, payload: SessionCreate) -> Session:
        session = Session(
            character_id=payload.character_id,
            title=payload.title or "Conversation",
            game_context=payload.game_context,
        )
        await self._put_session(session)
        return session

    async def save_session(self, session: Session) -> Session:
        session.updated_at = utcnow()
        await self._put_session(session)
        return session

    async def delete_session(self, session_id: str) -> bool:
        cur = await self.db.execute("DELETE FROM sessions WHERE id = ?", (session_id,))
        await self.db.commit()
        return cur.rowcount > 0

    async def append_message(self, session: Session, message: ChatMessage) -> Session:
        session.messages.append(message)
        return await self.save_session(session)

    async def _put_session(self, session: Session) -> None:
        await self.db.execute(
            """
            INSERT INTO sessions (id, character_id, data) VALUES (?, ?, ?)
            ON CONFLICT(id) DO UPDATE SET character_id = excluded.character_id, data = excluded.data
            """,
            (session.id, session.character_id, session.model_dump_json()),
        )
        await self.db.commit()


def parse_iso(value: str) -> datetime:
    return datetime.fromisoformat(value.replace("Z", "+00:00")).astimezone(timezone.utc)
