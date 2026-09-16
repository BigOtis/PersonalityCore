from __future__ import annotations

import asyncio
import logging
import time
from collections.abc import Awaitable, Callable
from typing import Any

from localtalker.config import AppConfig
from localtalker.inference import InferenceError, InferenceHub, gpu_info
from localtalker.jsonutil import extract_json_object, incremental_dialogue, parse_character_reply
from localtalker.models import (
    Character,
    CharacterCreate,
    CharacterReply,
    CharacterUpdate,
    ChatMessage,
    ChatRole,
    ConversationState,
    LiveServerMessage,
    Session,
    SessionCreate,
    TimingEvent,
    TimingReport,
    VoiceSettings,
    new_id,
)
from localtalker.prompts import to_openai_messages
from localtalker.speech import SpeechService, decode_pcm16, encode_pcm16
from localtalker.store import Store
from localtalker import __version__

log = logging.getLogger("localtalker.runtime")

Emit = Callable[[LiveServerMessage], Awaitable[None]]


class Clock:
    def __init__(self) -> None:
        self.origin = time.perf_counter()
        self.events: list[TimingEvent] = []

    def mark(self, name: str, detail: str | None = None) -> float:
        at_ms = (time.perf_counter() - self.origin) * 1000
        self.events.append(TimingEvent(name=name, at_ms=at_ms, detail=detail))
        return at_ms

    def report(self) -> TimingReport:
        totals: dict[str, float] = {}
        by_name = {event.name: event.at_ms for event in self.events}
        pairs = [
            ("transcription_ms", "speech_complete", "transcript_ready"),
            ("prompt_to_first_token_ms", "generation_start", "first_token"),
            ("first_dialogue_ms", "generation_start", "first_dialogue"),
            ("generation_ms", "generation_start", "generation_done"),
            ("first_audio_ms", "generation_start", "first_audio"),
            ("speech_to_audio_ms", "speech_complete", "first_audio"),
            ("speech_synthesis_ms", "tts_start", "tts_done"),
            ("turn_ms", "turn_start", "turn_done"),
        ]
        for key, start, end in pairs:
            if start in by_name and end in by_name:
                totals[key] = by_name[end] - by_name[start]
        return TimingReport(started_at_ms=0, events=self.events, totals_ms=totals)


class ConversationRuntime:
    def __init__(self, cfg: AppConfig, store: Store):
        from localtalker.scenes import SceneManager
        self.scenes = SceneManager(self)
        self.cfg = cfg
        self.store = store
        self.inference = InferenceHub(cfg)
        self.speech = SpeechService(cfg.speech)
        self.state = ConversationState.idle
        self.active_session_id: str | None = None
        self.active_character_id: str | None = None
        self._turns: dict[str, tuple[asyncio.Task, asyncio.Event]] = {}
        self._audio_buffers: dict[str, bytearray] = {}
        self._audio_meta: dict[str, tuple[int, int]] = {}
        self._warmup_task: asyncio.Task | None = None

    async def startup(self) -> None:
        await self.store.connect()
        await self.scenes.initialize()
        self._warmup_task = asyncio.create_task(self._warmup(), name="localtalker-warmup")

    async def _warmup(self) -> None:
        try:
            await self.speech.warmup()
        except Exception as exc:
            log.warning("Speech warmup failed: %s", exc)
        try:
            await self.inference.probe_all()
        except Exception as exc:
            log.warning("Provider probe failed: %s", exc)

    async def shutdown(self) -> None:
        await self.scenes.close()
        await self.interrupt()
        if self._warmup_task:
            self._warmup_task.cancel()
            await asyncio.gather(self._warmup_task, return_exceptions=True)
        await self.inference.close()
        await self.store.close()

    async def status(self) -> dict[str, Any]:
        probes = await self.inference.probe_all()
        errors = [item["error"] for item in probes if item.get("error") and not item.get("reachable")]
        speech = self.speech.status()
        if speech.get("error"):
            errors.append(speech["error"])
        ready = any(item["reachable"] and item["provider"]["kind"] != "mock" for item in probes)
        return {
            "ready": True,
            "inference_ready": ready,
            "active_sessions": list(self._turns),
            "version": __version__,
            "default_provider_id": self.cfg.preferred_provider_id,
            "state": self.state,
            "active_session_id": self.active_session_id,
            "active_character_id": self.active_character_id,
            "providers": [item["provider"] for item in probes],
            "provider_status": probes,
            "speech": speech,
            "gpu": await asyncio.to_thread(gpu_info),
            "errors": [err for err in errors if err],
        }

    async def ensure_session(self, character_id: str, session_id: str | None = None) -> Session:
        if session_id:
            session = await self.store.get_session(session_id)
            if not session:
                raise KeyError(f"Unknown session {session_id}")
            return session
        sessions = await self.store.list_sessions(character_id)
        if sessions:
            return sessions[0]
        return await self.store.create_session(SessionCreate(character_id=character_id))

    async def handle_live(self, session_id: str, payload: dict[str, Any], emit: Emit) -> None:
        kind = payload.get("type")
        if kind == "ping":
            await emit(LiveServerMessage(type="pong"))
            return
        if kind == "interrupt":
            await self.interrupt(emit, session_id)
            return
        if kind == "set_context":
            session = await self.store.get_session(session_id)
            if not session:
                await emit(LiveServerMessage(type="error", error="Unknown session"))
                return
            session.game_context = payload.get("game_context") or {}
            await self.store.save_session(session)
            await emit(LiveServerMessage(type="context", extra={"game_context": session.game_context}))
            return
        if kind == "audio":
            chunk = decode_pcm16(payload.get("pcm16_b64") or "")
            buffer = self._audio_buffers.setdefault(session_id, bytearray())
            if len(buffer) + len(chunk) > 8_000_000:
                self._audio_buffers.pop(session_id, None)
                raise ValueError("Recording too long. Keep each utterance below 60 seconds.")
            buffer.extend(chunk)
            self._audio_meta[session_id] = (int(payload.get("sample_rate") or 16000), int(payload.get("channels") or 1))
            if self.state == ConversationState.idle:
                await self._set_state(ConversationState.listening, emit)
            return
        if kind == "end_audio":
            await self._finish_audio(session_id, emit)
            return
        if kind == "text":
            text = (payload.get("text") or "").strip()
            if not text:
                return
            await self.run_turn(session_id, text, emit, source="text")
            return

    async def interrupt(self, emit: Emit | None = None, session_id: str | None = None) -> None:
        selected = [(sid, pair) for sid, pair in self._turns.items() if session_id is None or sid == session_id]
        tasks = []
        for sid, (task, cancel) in selected:
            cancel.set()
            if task is not asyncio.current_task():
                task.cancel()
                tasks.append(task)
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
        if session_id:
            self._audio_buffers.pop(session_id, None)
            self._audio_meta.pop(session_id, None)
        if not selected and emit:
            await emit(LiveServerMessage(type="cancelled"))
            await self._set_state(ConversationState.idle, emit)

    def is_busy(self, session_id: str) -> bool:
        return session_id in self._turns

    async def run_turn(self, session_id: str, user_text: str, emit: Emit,
                       source: str = "text", transcribe_ms: float | None = None) -> CharacterReply:
        if self.is_busy(session_id):
            raise ValueError("This conversation is busy. Interrupt before starting another turn.")
        cancel = asyncio.Event()
        task = asyncio.create_task(self._run_turn(session_id, user_text, emit, source, transcribe_ms, cancel))
        self._turns[session_id] = (task, cancel)
        try:
            return await task
        except asyncio.CancelledError:
            await emit(LiveServerMessage(type="cancelled"))
            await self._set_state(ConversationState.idle, emit)
            return CharacterReply()
        finally:
            self._turns.pop(session_id, None)
            if not self._turns:
                self.active_session_id = self.active_character_id = None

    async def _run_turn(
        self,
        session_id: str,
        user_text: str,
        emit: Emit,
        source: str,
        transcribe_ms: float | None,
        cancel: asyncio.Event,
    ) -> CharacterReply:
        clock = Clock()
        if transcribe_ms is not None:
            clock.origin -= transcribe_ms / 1000
            clock.events.extend([TimingEvent(name="speech_complete", at_ms=0), TimingEvent(name="transcript_ready", at_ms=transcribe_ms)])
        clock.mark("turn_start", source)
        session = await self.store.get_session(session_id)
        if not session:
            raise KeyError(f"Unknown session {session_id}")
        character = await self.store.get_character(session.character_id)
        if not character:
            raise KeyError(f"Unknown character {session.character_id}")
        self.active_session_id = session.id
        self.active_character_id = character.id

        user_message = ChatMessage(role=ChatRole.user, content=user_text)
        session = await self.store.append_message(session, user_message)
        await emit(
            LiveServerMessage(
                type="user_message",
                text=user_text,
                message_id=user_message.id,
                extra={"source": source},
            )
        )

        if character.provider_id and not any(p.id == character.provider_id for p in self.cfg.providers):
            raise InferenceError(f"Unknown provider {character.provider_id}. Edit this character to choose a provider.")
        preferred = self.cfg.provider(character.provider_id)
        await self._set_state(ConversationState.thinking, emit)
        try:
            provider = await self.inference.resolve(preferred, character.model)
        except InferenceError as exc:
            await self._fail(str(exc), emit, clock)
            raise

        await self._set_state(ConversationState.thinking, emit)
        clock.mark("generation_start", provider.id)
        messages = to_openai_messages(character, session)
        if character.temperature is not None:
            provider = provider.model_copy(update={"temperature": character.temperature})
        backend = self.inference.get_backend(provider)
        raw_parts: list[str] = []
        spoken_so_far = ""
        first_token = False
        first_dialogue = False
        assistant_id = new_id("msg")
        tts_task: asyncio.Task[None] | None = None
        spoken_queue: asyncio.Queue[str | None] = asyncio.Queue()

        async def tts_worker() -> None:
            clock.mark("tts_start")
            first_audio = False
            index = 0
            while True:
                sentence = await spoken_queue.get()
                if sentence is None:
                    break
                if cancel.is_set():
                    break
                try:
                    async for chunk in self.speech.synthesize_stream(
                        sentence,
                        character.voice.voice_id,
                        character.voice.speed,
                    ):
                        if cancel.is_set():
                            break
                        if not first_audio:
                            clock.mark("first_audio")
                            first_audio = True
                            await self._set_state(ConversationState.speaking, emit)
                        await emit(
                            LiveServerMessage(
                                type="audio",
                                pcm16_b64=encode_pcm16(chunk.pcm16),
                                sample_rate=chunk.sample_rate,
                                chunk_index=index,
                                voice_id=character.voice.voice_id,
                            )
                        )
                        index += 1
                except Exception as exc:
                    log.warning("TTS chunk failed: %s", exc)
                    await emit(LiveServerMessage(type="warning", error=f"Voice synthesis failed: {exc}"))
            clock.mark("tts_done")

        try:
            if self.cfg.speech.tts_enabled:
                tts_task = asyncio.create_task(tts_worker())
            async for chunk in backend.stream(messages, cancel):
                if chunk.delta:
                    raw_parts.append(chunk.delta)
                    if not first_token:
                        clock.mark("first_token")
                        first_token = True
                    await emit(
                        LiveServerMessage(
                            type="token",
                            delta=chunk.delta,
                            text="".join(raw_parts),
                            message_id=assistant_id,
                        )
                    )
                    dialogue = incremental_dialogue("".join(raw_parts)) or ""
                    if dialogue:
                        if not first_dialogue:
                            clock.mark("first_dialogue")
                            first_dialogue = True
                        await emit(
                            LiveServerMessage(
                                type="dialogue",
                                text=dialogue,
                                message_id=assistant_id,
                            )
                        )
                        new_speech = dialogue[len(spoken_so_far) :]
                        ready, leftover = _take_sentences(new_speech)
                        if ready:
                            spoken_so_far += ready
                            await spoken_queue.put(ready)
                if chunk.done:
                    break
            clock.mark("generation_done")
            raw = "".join(raw_parts)
            if cancel.is_set():
                raise asyncio.CancelledError()
            if not raw.strip():
                raise InferenceError("The model returned an empty response. Try a different model or shorter history.")
            if source == "scene" and extract_json_object(raw) is None:
                raise InferenceError("The scene model returned unstructured text instead of dialogue JSON; no speech was queued from that text.")
            reply = parse_character_reply(raw)
            leftover = reply.dialogue[len(spoken_so_far):].strip()
            if tts_task:
                if leftover:
                    await spoken_queue.put(leftover)
                await spoken_queue.put(None)
                await tts_task
        except asyncio.CancelledError:
            cancel.set()
            raise
        except Exception as exc:
            cancel.set()
            log.exception("Generation failed for session %s", session_id)
            await self._fail(f"Generation failed: {str(exc) or type(exc).__name__}", emit, clock)
            raise
        finally:
            if tts_task and not tts_task.done():
                tts_task.cancel()
                await asyncio.gather(tts_task, return_exceptions=True)
            await backend.close()


        if cancel.is_set():
            reply.dialogue = reply.dialogue or "".join(raw_parts)
            await emit(LiveServerMessage(type="cancelled", reply=reply, message_id=assistant_id))
            await self._set_state(ConversationState.idle, emit)
            return reply

        session = await self.store.get_session(session_id)
        if session is None:
            raise KeyError("Session was deleted during generation")
        if reply.state_changes:
            session.character_state = {**session.character_state, **reply.state_changes}
        assistant = ChatMessage(
            id=assistant_id,
            role=ChatRole.assistant,
            content=reply.dialogue,
            reply=reply,
        )
        session = await self.store.append_message(session, assistant)
        clock.mark("turn_done")
        timing = clock.report()
        await emit(
            LiveServerMessage(
                type="reply",
                text=reply.dialogue,
                reply=reply,
                timing=timing,
                message_id=assistant_id,
                extra={"provider_id": provider.id, "model": provider.model},
            )
        )
        await self._set_state(ConversationState.idle, emit)
        return reply


    async def preview_voice(self, voice_id: str, text: str, speed: float = 1.0) -> dict[str, Any]:
        chunks = await self.speech.synthesize(text, voice_id, speed)
        pcm = b"".join(chunk.pcm16 for chunk in chunks)
        rate = chunks[0].sample_rate if chunks else 24000
        return {
            "voice_id": voice_id,
            "sample_rate": rate,
            "pcm16_b64": encode_pcm16(pcm),
        }

    async def apply_config(self, patch: dict[str, Any]) -> AppConfig:
        from localtalker.config import merge_config

        previous_model = self.cfg.llama_model_path
        self.cfg = merge_config(self.cfg, patch)
        self.inference.cfg = self.cfg
        self.speech.cfg = self.cfg.speech
        if patch.get("llama_model_path") and patch["llama_model_path"] != previous_model:
            await self.inference.stop_llama_server()
        return self.cfg

    async def activate_recommended(self, model_id: str) -> dict[str, Any]:
        from localtalker.catalog import get_recommended
        from localtalker.install import start_install, job_status

        model = get_recommended(model_id)
        if not model.installed():
            return start_install(model_id)
        await self._use_recommended(model)
        return {"id": model.id, "status": "ready", "path": str(model.dest())}

    async def _use_recommended(self, model) -> None:
        from localtalker.config import save_config

        self.cfg.llama_model_path = str(model.dest())
        self.cfg.llama_ctx = min(self.cfg.llama_ctx, model.context)
        self.cfg.preferred_provider_id = "llamacpp"
        from localtalker.inference import _resolve_llama_server

        binary = _resolve_llama_server(self.cfg.llama_server_path, model.dest())
        if binary:
            self.cfg.llama_server_path = str(binary)
        for provider in self.cfg.providers:
            if provider.id == "llamacpp":
                provider.model = "muse-glimmer" if "glimmer" in model.id else model.filename
                provider.temperature = model.temperature
                provider.max_tokens = max(provider.max_tokens, 800)
                provider.enabled = True
        save_config(self.cfg)
        self.inference.cfg = self.cfg
        llamacpp = self.cfg.provider("llamacpp")
        if llamacpp:
            await self.inference.ensure_llama_server(llamacpp)

    async def _finish_audio(self, session_id: str, emit: Emit) -> None:
        pcm = bytes(self._audio_buffers.pop(session_id, bytearray()))
        sample_rate, channels = self._audio_meta.pop(session_id, (16000, 1))
        if not pcm:
            await self._set_state(ConversationState.idle, emit)
            return
        await self._set_state(ConversationState.transcribing, emit)
        started = time.perf_counter()
        try:
            result = await self.speech.transcribe_pcm16(pcm, sample_rate, channels)
        except Exception as exc:
            await emit(LiveServerMessage(type="error", error=f"Transcription failed: {exc}"))
            await self._set_state(ConversationState.idle, emit)
            return
        elapsed = (time.perf_counter() - started) * 1000
        text = result.text.strip()
        await emit(
            LiveServerMessage(
                type="transcript",
                transcript=text,
                extra={"duration_ms": result.duration_ms or elapsed},
            )
        )
        if not text:
            await emit(LiveServerMessage(type="warning", error="I could not hear a clear sentence. Try again?"))
            await self._set_state(ConversationState.idle, emit)
            return
        await self.run_turn(session_id, text, emit, source="speech", transcribe_ms=result.duration_ms or elapsed)

    async def _set_state(self, state: ConversationState, emit: Emit | None) -> None:
        self.state = state
        if emit:
            await emit(LiveServerMessage(type="state", state=state))

    async def _fail(self, message: str, emit: Emit, clock: Clock) -> None:
        log.warning(message)
        clock.mark("error", message)
        await emit(LiveServerMessage(type="error", error=message, timing=clock.report()))
        await self._set_state(ConversationState.error, emit)
        await self._set_state(ConversationState.idle, emit)


def _take_sentences(text: str) -> tuple[str, str]:
    import re

    matches = list(re.finditer(r"[.!?]", text))
    if not matches:
        return "", text
    last = matches[-1].end()
    return text[:last], text[last:]
