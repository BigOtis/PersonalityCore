"""Engine-independent, bounded multi-character conversation direction.

The host owns proximity, input and audible playback. This service owns shared
history, participation policy, goals and the speaking order. No engine types.
"""
from __future__ import annotations

import asyncio
import base64
import json
import time
from typing import Any, Literal

from pydantic import BaseModel, Field, model_validator

from localtalker.models import CharacterCreate, CharacterReply, CharacterUpdate, SessionCreate, new_id
from localtalker.speech import encode_pcm16


class SceneMember(BaseModel):
    key: str = Field(pattern=r"^[a-zA-Z0-9_-]+$", max_length=60)
    character: CharacterCreate
    character_id: str | None = None
    public_description: str = Field(default="", max_length=1200)
    backstory: str = Field(default="", max_length=4000)
    relationships: dict[str, str] = Field(default_factory=dict)


class GroupCreate(BaseModel):
    character_ids: list[str] = Field(min_length=2, max_length=12)
    title: str | None = Field(default=None, max_length=100)
    player_name: str = Field(default="You", max_length=100)

    @model_validator(mode="after")
    def distinct_characters(self):
        if len(set(self.character_ids)) != len(self.character_ids):
            raise ValueError("Group members must be distinct characters")
        return self


class SceneDefinition(BaseModel):
    name: str = Field(min_length=1, max_length=100)
    goal: str = Field(default="Have a useful conversation.", max_length=6000)
    guidance: str = Field(default="Keep each turn to one or two short sentences.", max_length=6000)
    members: list[SceneMember] = Field(min_length=2, max_length=12)
    context: dict[str, Any] = Field(default_factory=dict)
    join_policy: Literal["open", "invite", "ignore"] = "open"
    autonomous_turns: int = Field(default=6, ge=0, le=30)
    greet_on_join: bool = True

    @model_validator(mode="after")
    def distinct_keys(self):
        if len({m.key for m in self.members}) != len(self.members):
            raise ValueError("Scene member keys must be unique")
        return self


class SceneCommand(BaseModel):
    type: Literal["text", "audio", "end_audio", "interrupt", "playback_done", "presence", "join", "leave", "direct", "continue", "pause", "ping", "speak", "reset"]
    text: str = Field(default="", max_length=32000)
    target: str = "all"
    player_name: str = Field(default="Visitor", max_length=100)
    present: bool = True
    pcm16_b64: str = Field(default="", max_length=11000000)
    sample_rate: int = Field(default=16000, ge=8000, le=192000)
    channels: int = Field(default=1, ge=1, le=2)
    turn_id: str = ""
    goal: str | None = Field(default=None, max_length=6000)
    guidance: str | None = Field(default=None, max_length=6000)
    join_policy: Literal["open", "invite", "ignore"] | None = None
    turns: int = Field(default=4, ge=1, le=30)
    context: dict[str, Any] | None = None
    stop: bool = True


class Scene:
    def __init__(self, runtime, scene_id: str, definition: SceneDefinition, sessions: dict[str, str], history=None, kind: str = "scene"):
        self.runtime, self.id, self.definition, self.sessions = runtime, scene_id, definition, sessions
        self.history: list[dict] = history or []
        self.kind = kind if kind in {"scene", "group"} else "scene"
        self.subscribers: set[asyncio.Queue] = set()
        self.player_name, self.present, self.joined = "You" if self.kind == "group" else "Visitor", False, False
        self.task: asyncio.Task | None = None
        self.turn_id = ""
        self.speaker = ""
        self.state = "idle"
        self.cursor = 0
        self.playback_ack = asyncio.Event()
        self.audio = bytearray()
        self.audio_meta = (16000, 1)
        self.lock = asyncio.Lock()

    def snapshot(self):
        return {"id": self.id, "kind": self.kind, "definition": self.definition.model_dump(), "sessions": self.sessions,
                "history": self.history, "player_name": self.player_name, "present": self.present,
                "joined": self.joined, "speaker": self.speaker, "state": self.state, "turn_id": self.turn_id}

    def _spoken_history(self):
        """Lines the model may see. Scripted PA/GUIDE beats stay off the prompt."""
        return [line for line in self.history
                if not line.get("scripted") and line.get("speaker") != "announcer"]

    def _trim_history(self):
        if len(self.history) > 36:
            self.history = self.history[-24:]

    def _reply_reason(self) -> str:
        if self.definition.context.get("interaction_mode") == "companion":
            latest = next((line["text"] for line in reversed(self.history) if line.get("speaker") == "player"), "")
            return (
                f"Current player instruction: {latest!r}. Interpret ONLY this instruction. "
                "Earlier player instructions are history, not a queue: never replay them. "
                "Use earlier speech only to resolve references or corrections. "
                "Return one brief spoken response and only the actions this current instruction requests."
            )
        if self.kind == "group":
            return (
                f"Respond to {self.player_name}'s latest message as yourself in this group conversation. "
                "Do not repeat another character's answer."
            )
        return f"Respond to {self.player_name}'s latest contribution. Do not repeat another droid's answer."

    async def save(self):
        await self.runtime.store.db.execute("INSERT OR REPLACE INTO scenes(id,data) VALUES(?,?)",
            (self.id, json.dumps({"kind": self.kind, "player_name": self.player_name,
                "definition": self.definition.model_dump(), "sessions": self.sessions, "history": self.history})))
        await self.runtime.store.db.commit()

    async def update_definition(self, definition: SceneDefinition):
        """Revise an existing cast without discarding the shared conversation."""
        if {m.key for m in definition.members} != set(self.sessions):
            raise ValueError("Keep member keys unchanged when updating a scene; create a new scene to change its cast")
        async with self.lock:
            await self.stop()
            for member in definition.members:
                session = await self.runtime.store.get_session(self.sessions[member.key])
                await self.runtime.store.update_character(session.character_id, CharacterUpdate.model_validate(member.character.model_dump(exclude_none=True)))
            self.definition = definition
            if definition.join_policy == "ignore": self.joined = False
            await self.save()
            await self.emit({"type": "scene", "scene": self.snapshot()})

    async def emit(self, event: dict):
        event = {"scene_id": self.id, **event}
        for queue in tuple(self.subscribers):
            if queue.full():
                # Disconnect slow consumers instead of accumulating unbounded PCM.
                self.subscribers.discard(queue)
                while not queue.empty(): queue.get_nowait()
                queue.put_nowait({"type": "error", "error": "Scene consumer fell behind. Reconnect."})
            else:
                queue.put_nowait(event)

    async def stop(self):
        task, self.task = self.task, None
        if task and not task.done():
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        self.turn_id, self.speaker, self.state = "", "", "idle"
        self.audio.clear()
        await self.emit({"type": "interrupted"})
        await self.emit({"type": "scene_state", "state": "idle"})

    def schedule(self, targets: list[str], reason: str, *, speech_only: bool = False):
        self.task = asyncio.create_task(self.run(targets, reason, speech_only=speech_only), name=f"scene-{self.id}")

    def order(self, count: int):
        keys = list(self.sessions)
        result = [keys[(self.cursor + i) % len(keys)] for i in range(count)]
        self.cursor = (self.cursor + count) % len(keys)
        return result

    async def command(self, command: SceneCommand):
        if command.type == "playback_done":
            if command.turn_id == self.turn_id:
                self.playback_ack.set()
            return
        if command.type == "ping":
            await self.emit({"type": "pong"})
            return
        async with self.lock:
            kind = command.type
            if command.target != "all" and command.target not in self.sessions:
                raise ValueError("Unknown conversation target")
            if kind in {"interrupt", "pause"}:
                await self.stop()
            elif kind == "reset":
                await self.stop()
                self.history.clear()
                self.definition.context = {}
                self.cursor = 0
                for session_id in self.sessions.values():
                    session = await self.runtime.store.get_session(session_id)
                    session.messages = []
                    session.game_context = {}
                    session.character_state = {}
                    await self.runtime.store.save_session(session)
                await self.save()
                await self.emit({"type": "scene", "scene": self.snapshot()})
            elif kind == "audio":
                data = base64.b64decode(command.pcm16_b64, validate=True)
                if len(self.audio) + len(data) > 8_000_000:
                    self.audio.clear()
                    raise ValueError("Recording exceeds scene audio limit")
                self.audio.extend(data)
                self.audio_meta = command.sample_rate, command.channels
            elif kind == "end_audio":
                pcm, self.audio = bytes(self.audio), bytearray()
                if not self.joined:
                    raise ValueError("Join this conversation before speaking")
                await self.stop()
                self.task = asyncio.create_task(self.transcribe(pcm, command.target))
            elif kind == "text":
                if not self.joined:
                    raise ValueError("Join this conversation before speaking")
                if command.text.strip():
                    await self.stop()
                    await self.player_turn(command.text.strip(), command.target)
            elif kind in {"presence", "join", "leave"}:
                if kind == "leave" or not command.present:
                    self.present = self.joined = False
                else:
                    newly_present = not self.present
                    self.present = True
                    self.player_name = command.player_name
                    allowed = self.definition.join_policy == "open" or (kind == "join" and self.definition.join_policy == "invite")
                    new_join = allowed and not self.joined
                    self.joined = allowed
                    if new_join and self.kind != "group" and self.definition.greet_on_join:
                        await self.stop()
                        self.schedule([self.order(1)[0]], f"{self.player_name} approached and joined. Briefly acknowledge them and invite their view on the goal.")
                    elif newly_present and self.definition.join_policy == "ignore":
                        await self.emit({"type": "notice", "text": "The droids notice your approach but keep their discussion private."})
                await self.emit({"type": "participation", "joined": self.joined, "present": self.present, "policy": self.definition.join_policy, "player_name": self.player_name})
            elif kind == "direct":
                if command.stop:
                    await self.stop()
                if command.goal is not None: self.definition.goal = command.goal
                if command.guidance is not None: self.definition.guidance = command.guidance
                if command.context is not None: self.definition.context = command.context
                if command.join_policy is not None:
                    self.definition.join_policy = command.join_policy
                    if command.join_policy == "ignore": self.joined = False
                context_only = (command.context is not None and not command.stop
                    and command.goal is None and command.guidance is None and command.join_policy is None)
                if context_only:
                    await self.emit({"type": "context", "ok": True})
                    return
                await self.save()
                await self.emit({"type": "scene", "scene": self.snapshot()})
            elif kind == "continue":
                await self.stop()
                if command.target == "all":
                    self.schedule(self.order(command.turns), "Converse with the other characters. Respond naturally to the latest exchange. Once a practical point is settled, develop its consequences or the relationship instead of repeating the plan.")
                else:
                    # A host may ask one character for an unprompted line, with a
                    # direction the other characters never see.
                    self.schedule([command.target], command.text.strip() or "Say one short unprompted line in your own voice.", speech_only=self.definition.context.get("interaction_mode") == "companion")
            elif kind == "speak":
                # Host-authored line: TTS and history only. No model turn.
                key = command.target if command.target != "all" else next(iter(self.sessions))
                if key not in self.sessions:
                    raise ValueError("Unknown conversation target")
                if not command.text.strip():
                    return
                await self.stop()
                self.task = asyncio.create_task(self.speak_line(key, command.text.strip()), name=f"speak-{self.id}")

    async def speak_line(self, key: str, text: str):
        """Speak a predetermined line as this character. Interruptible. No inference."""
        member = next(m for m in self.definition.members if m.key == key)
        session = await self.runtime.store.get_session(self.sessions[key])
        live = await self.runtime.store.get_character(session.character_id)
        speaker_name = live.name if live else member.character.name
        voice = live.voice if live else member.character.voice
        self.turn_id, self.speaker = new_id("turn"), key
        self.playback_ack = asyncio.Event()
        self.state = "speaking"
        try:
            await self.emit({"type": "turn_start", "turn_id": self.turn_id, "speaker": key, "name": speaker_name,
                             "remaining": 0, "reason": "scripted"})
            await self.emit({"type": "dialogue", "turn_id": self.turn_id, "speaker": key, "name": speaker_name, "text": text})
            audio_seconds = 0.0
            if self.runtime.cfg.speech.tts_enabled:
                index = 0
                async for chunk in self.runtime.speech.synthesize_stream(text, voice.voice_id, voice.speed):
                    if asyncio.current_task().cancelling():
                        raise asyncio.CancelledError()
                    audio_seconds += len(chunk.pcm16) / (2 * chunk.sample_rate)
                    await self.emit({"type": "audio", "turn_id": self.turn_id, "speaker": key, "name": speaker_name,
                                     "pcm16_b64": encode_pcm16(chunk.pcm16), "sample_rate": chunk.sample_rate,
                                     "chunk_index": index, "voice_id": voice.voice_id})
                    index += 1
            reply = CharacterReply.from_dialogue(text)
            self.history.append({"speaker": key, "name": speaker_name, "text": text,
                                 "reply": reply.model_dump(), "turn_id": self.turn_id, "scripted": True})
            self._trim_history()
            await self.save()
            await self.emit({"type": "reply", "turn_id": self.turn_id, "speaker": key, "name": speaker_name,
                             "text": text, "reply": reply.model_dump()})
            await self.emit({"type": "turn_end", "turn_id": self.turn_id, "speaker": key, "name": speaker_name,
                             "audio_seconds": audio_seconds, "awaiting_playback": bool(audio_seconds)})
            if audio_seconds:
                try:
                    await asyncio.wait_for(self.playback_ack.wait(), timeout=audio_seconds + 3)
                except TimeoutError:
                    pass
            self.speaker, self.turn_id, self.state = "", "", "idle"
            await self.emit({"type": "scene_state", "state": "idle", "text": "Your turn" if self.joined else "Discussion paused"})
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            self.state = "idle"
            await self.emit({"type": "error", "error": str(exc)})
            await self.emit({"type": "scene_state", "state": "idle"})

    async def player_turn(self, text, target):
        self.history.append({"speaker": "player", "name": self.player_name, "text": text, "target": target})
        self._trim_history()
        await self.save()
        await self.emit({"type": "player_message", "text": text, "target": target, "name": self.player_name})
        targets = self.order(len(self.sessions)) if target == "all" else [target]
        self.schedule(targets, self._reply_reason())

    async def transcribe(self, pcm, target):
        try:
            await self.emit({"type": "scene_state", "state": "transcribing"})
            result = await self.runtime.speech.transcribe_pcm16(pcm, *self.audio_meta)
            text = result.text.strip()
            await self.emit({"type": "transcript", "text": text, "transcription_ms": result.duration_ms})
            if not text: raise ValueError("No clear speech detected. Hold T and try again, or use text input.")
            # Hand off without replacing the task while cancellation still owns it.
            self.history.append({"speaker": "player", "name": self.player_name, "text": text, "target": target})
            self._trim_history()
            await self.save()
            await self.emit({"type": "player_message", "text": text, "target": target, "name": self.player_name})
            await self.run(self.order(len(self.sessions)) if target == "all" else [target], self._reply_reason())
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            await self.emit({"type": "error", "error": str(exc)})
            await self.emit({"type": "scene_state", "state": "idle"})

    async def run(self, targets: list[str], reason: str, *, speech_only: bool = False):
        heard_this_round: set[str] = set()
        invited = set(targets)
        try:
            for index, key in enumerate(targets):
                self.turn_id, self.speaker = new_id("turn"), key
                self.playback_ack = asyncio.Event()
                self.state = "thinking"
                member = next(m for m in self.definition.members if m.key == key)
                session = await self.runtime.store.get_session(self.sessions[key])
                live = await self.runtime.store.get_character(session.character_id)
                speaker_name = live.name if live else member.character.name
                # The shared ledger is authoritative; avoid accumulating director prompts
                # in each actor's private chat and eventually exhausting its context window.
                session.messages = []
                companion = self.definition.context.get("interaction_mode") == "companion"
                recent = self._spoken_history()[-12:]
                if companion:
                    # Keep the latest exchange intact, then fit older speech into
                    # a small budget. Persistent notes and actual state live in host context.
                    recent = recent[-4:]
                    while len(recent) > 2 and sum(len(line.get("text", "")) for line in recent) > 900:
                        recent = recent[1:]
                session.game_context = {**self.definition.context, "conversation": {
                    "goal": self.definition.goal, "guidance": self.definition.guidance,
                    "self": {"key": key, "name": speaker_name, "backstory": member.backstory,
                             "relationships": member.relationships},
                    "participants": [{"key": m.key, "name": m.character.name,
                                      "description": m.public_description, "character_id": m.character_id} for m in self.definition.members],
                    "player": {"name": self.player_name, "present": self.present, "joined": self.joined},
                    "recent_dialogue": [{k: line[k] for k in ("speaker", "name", "text") if k in line} for line in recent], "current_speaker": key,
                    "next_speaker": targets[index + 1] if index + 1 < len(targets) else "player" if self.joined else "pause",
                }}
                if companion:
                    brief = session.game_context["conversation"]
                    brief.pop("goal", None)
                    brief.pop("guidance", None)
                    brief.pop("self", None)
                    brief["participants"] = [{"key": m.key, "name": m.character.name} for m in self.definition.members]
                if speech_only:
                    session.game_context["speech_only"] = True
                await self.runtime.store.save_session(session)
                await self.emit({"type": "turn_start", "turn_id": self.turn_id, "speaker": key, "name": speaker_name,
                                 "remaining": len(targets) - index - 1, "reason": reason})
                audio_seconds = 0.0

                async def forward(event):
                    nonlocal audio_seconds
                    value = event.model_dump(mode="json", exclude_none=True)
                    if event.type in {"user_message", "cancelled"}: return
                    if speech_only and event.type == "reply" and value.get("reply"):
                        value["reply"]["actions"] = []
                        value["reply"]["world_interactions"] = []
                    if event.type == "audio":
                        audio_seconds += len(base64.b64decode(event.pcm16_b64)) / (2 * event.sample_rate)
                    if event.type == "state": self.state = event.state.value
                    await self.emit({**value, "turn_id": self.turn_id, "speaker": key, "name": speaker_name})

                reply = await self.runtime.run_turn(session.id, reason, forward, source="scene")
                if speech_only:
                    reply.actions = []
                    reply.world_interactions = []
                heard_this_round.add(key)
                if asyncio.current_task().cancelling(): raise asyncio.CancelledError()
                if not reply.dialogue: break
                self.history.append({"speaker": key, "name": speaker_name, "text": reply.dialogue,
                                     "reply": reply.model_dump(), "turn_id": self.turn_id})
                self._trim_history()
                await self.save()
                self.state = "speaking" if audio_seconds else "idle"
                # Preserve the bounded/fair roster, but let a named peer answer a
                # question before an unrelated scheduled speaker takes the floor.
                if reply.addressee and reply.addressee != key:
                    try:
                        addressed_index = targets.index(reply.addressee, index + 1)
                        targets[index + 1], targets[addressed_index] = targets[addressed_index], targets[index + 1]
                        # Do not satisfy an invitation by stranding one character
                        # with consecutive turns at the tail of the discussion.
                        if any(targets[j] == targets[j + 1] for j in range(index, len(targets) - 1)):
                            targets[index + 1], targets[addressed_index] = targets[addressed_index], targets[index + 1]
                    except ValueError:
                        pass
                await self.emit({"type": "turn_end", "turn_id": self.turn_id, "speaker": key, "name": speaker_name,
                                 "audio_seconds": audio_seconds, "awaiting_playback": bool(audio_seconds)})
                if audio_seconds:
                    try:
                        await asyncio.wait_for(self.playback_ack.wait(), timeout=audio_seconds + 3)
                    except TimeoutError:
                        await self.emit({"type": "notice", "text": "Playback acknowledgement timed out; continuing the scene."})
                await asyncio.sleep(0.35)
                if reply.addressee == "player" and self.joined:
                    # An explicit invitation hands the floor to the human, even
                    # when other characters still have scheduled turns.
                    break
                if reply.conversation_complete and heard_this_round == invited and reply.addressee not in invited:
                    await self.emit({"type": "notice", "text": "The conversation reached a natural pause."})
                    break
            self.speaker, self.turn_id, self.state = "", "", "idle"
            await self.emit({"type": "scene_state", "state": "idle", "text": "Your turn" if self.joined else "Discussion paused"})
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            self.state = "idle"
            await self.emit({"type": "error", "error": str(exc)})
            await self.emit({"type": "scene_state", "state": "idle"})


class SceneManager:
    def __init__(self, runtime):
        self.runtime = runtime
        self.scenes: dict[str, Scene] = {}

    async def initialize(self):
        await self.runtime.store.db.execute("CREATE TABLE IF NOT EXISTS scenes(id TEXT PRIMARY KEY,data TEXT NOT NULL)")
        await self.runtime.store.db.commit()
        cursor = await self.runtime.store.db.execute("SELECT id,data FROM scenes")
        for row in await cursor.fetchall():
            data = json.loads(row["data"])
            scene = Scene(
                self.runtime,
                row["id"],
                SceneDefinition.model_validate(data["definition"]),
                data["sessions"],
                data["history"],
                data.get("kind", "scene"),
            )
            if data.get("player_name"):
                scene.player_name = data["player_name"]
            self.scenes[row["id"]] = scene

    async def create(self, definition):
        sessions = {}
        for member in definition.members:
            character = await self.runtime.store.create_character(member.character)
            session = await self.runtime.store.create_session(SessionCreate(character_id=character.id, title=definition.name))
            sessions[member.key] = session.id
        scene = Scene(self.runtime, new_id("scene"), definition, sessions)
        self.scenes[scene.id] = scene
        await scene.save()
        return scene

    async def create_from_characters(self, character_ids: list[str], title: str | None = None, player_name: str = "You"):
        characters = []
        for character_id in character_ids:
            character = await self.runtime.store.get_character(character_id)
            if not character:
                raise ValueError(f"Unknown character {character_id}")
            characters.append(character)
        names = [item.name for item in characters]
        if len(names) == 2:
            heading = f"{names[0]} & {names[1]}"
        else:
            heading = ", ".join(names[:-1]) + f" & {names[-1]}"
        heading = (title or heading)[:100]
        members = []
        sessions = {}
        for character in characters:
            member = SceneMember(
                key=character.id,
                character_id=character.id,
                character=CharacterCreate(
                    name=character.name,
                    personality=character.personality,
                    instructions=character.instructions,
                    provider_id=character.provider_id,
                    model=character.model,
                    temperature=character.temperature,
                    voice=character.voice,
                    color=character.color,
                ),
                public_description=(character.personality or character.instructions)[:1200],
            )
            session = await self.runtime.store.create_session(
                SessionCreate(
                    character_id=character.id,
                    title=heading,
                    game_context={"group_chat": True},
                )
            )
            sessions[member.key] = session.id
            members.append(member)
        definition = SceneDefinition(
            name=heading,
            goal="Have a natural spoken conversation with the player and the other characters.",
            guidance="Keep each turn to one or two short spoken sentences. React to the other speakers. Do not repeat someone else's line.",
            members=members,
            join_policy="open",
            autonomous_turns=len(members),
        )
        scene = Scene(self.runtime, new_id("scene"), definition, sessions, kind="group")
        scene.player_name = player_name or "You"
        self.scenes[scene.id] = scene
        await scene.save()
        return scene

    async def delete(self, scene_id: str) -> bool:
        scene = self.scenes.pop(scene_id, None)
        if not scene:
            return False
        await scene.stop()
        for session_id in scene.sessions.values():
            await self.runtime.store.delete_session(session_id)
        await self.runtime.store.db.execute("DELETE FROM scenes WHERE id = ?", (scene_id,))
        await self.runtime.store.db.commit()
        return True

    async def close(self):
        await asyncio.gather(*(scene.stop() for scene in self.scenes.values()))
