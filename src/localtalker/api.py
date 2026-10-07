from __future__ import annotations

import asyncio
import json
import logging
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field, ValidationError
from localtalker.config import AppConfig

from localtalker.models import (
    CharacterCreate,
    CharacterUpdate,
    LiveServerMessage,
    LiveClientMessage,
    SessionCreate,
)
from localtalker.runtime import ConversationRuntime
from localtalker.store import Store
from localtalker.scenes import GroupCreate, SceneDefinition, SceneCommand

log = logging.getLogger("localtalker.api")


class ContextPatch(BaseModel):
    game_context: dict[str, Any] = Field(default_factory=dict)
    character_state: dict[str, Any] | None = None


class VoicePreviewRequest(BaseModel):
    voice_id: str = "af_bella"
    text: str = Field(default="The road is long, but the fire is warm.", max_length=2000)
    speed: float = Field(default=1.0, ge=0.5, le=2.0)


class TextTurnRequest(BaseModel):
    text: str = Field(min_length=1, max_length=32000)


class ModelInstallRequest(BaseModel):
    id: str = "muse-glimmer"


def create_app(runtime: ConversationRuntime, static_dir: Path | None = None, start: bool = True) -> FastAPI:
    @asynccontextmanager
    async def lifespan(_app: FastAPI):
        if start:
            await runtime.startup()
        try:
            yield
        finally:
            if start:
                await runtime.shutdown()

    app = FastAPI(
        title="PersonalityCore",
        version="0.2.0",
        lifespan=lifespan,
    )
    app.add_middleware(
        CORSMiddleware,
        allow_origins=["http://127.0.0.1:5173", "http://localhost:5173"],
        allow_methods=["*"],
        allow_headers=["*"],
    )
    app.state.runtime = runtime

    @app.get("/v1/health")
    async def health() -> dict[str, Any]:
        return {"ok": True, "ready": True}

    @app.get("/v1/status")
    async def status() -> dict[str, Any]:
        return await runtime.status()

    @app.get("/v1/config")
    async def get_config() -> dict[str, Any]:
        return json.loads(runtime.cfg.model_dump_json())

    @app.patch("/v1/config")
    async def patch_config(payload: dict[str, Any]) -> dict[str, Any]:
        try:
            cfg = await runtime.apply_config(payload)
        except (ValidationError, ValueError) as exc:
            raise HTTPException(422, str(exc)) from exc
        return json.loads(cfg.model_dump_json())

    @app.get("/v1/characters")
    async def list_characters() -> list[dict[str, Any]]:
        return [json.loads(c.model_dump_json()) for c in await runtime.store.list_characters()]

    @app.post("/v1/characters", status_code=201)
    async def create_character(payload: CharacterCreate) -> dict[str, Any]:
        character = await runtime.store.create_character(payload)
        return json.loads(character.model_dump_json())

    @app.get("/v1/characters/{character_id}")
    async def get_character(character_id: str) -> dict[str, Any]:
        character = await runtime.store.get_character(character_id)
        if not character:
            raise HTTPException(404, "Character not found")
        return json.loads(character.model_dump_json())

    @app.patch("/v1/characters/{character_id}")
    async def update_character(character_id: str, payload: CharacterUpdate) -> dict[str, Any]:
        character = await runtime.store.update_character(character_id, payload)
        if not character:
            raise HTTPException(404, "Character not found")
        return json.loads(character.model_dump_json())

    @app.delete("/v1/characters/{character_id}")
    async def delete_character(character_id: str) -> dict[str, bool]:
        for session in await runtime.store.list_sessions(character_id):
            if runtime.is_busy(session.id):
                raise HTTPException(409, "Interrupt this character before deleting it")
        if not await runtime.store.delete_character(character_id):
            raise HTTPException(404, "Character not found")
        return {"ok": True}

    @app.get("/v1/sessions")
    async def list_sessions(character_id: str | None = None) -> list[dict[str, Any]]:
        return [json.loads(s.model_dump_json()) for s in await runtime.store.list_sessions(character_id)]

    @app.post("/v1/sessions", status_code=201)
    async def create_session(payload: SessionCreate) -> dict[str, Any]:
        if not await runtime.store.get_character(payload.character_id):
            raise HTTPException(404, "Character not found")
        session = await runtime.store.create_session(payload)
        return json.loads(session.model_dump_json())

    @app.get("/v1/sessions/{session_id}")
    async def get_session(session_id: str) -> dict[str, Any]:
        session = await runtime.store.get_session(session_id)
        if not session:
            raise HTTPException(404, "Session not found")
        return json.loads(session.model_dump_json())

    @app.delete("/v1/sessions/{session_id}")
    async def delete_session(session_id: str) -> dict[str, bool]:
        if runtime.is_busy(session_id):
            raise HTTPException(409, "Interrupt the conversation before deleting it")
        if not await runtime.store.delete_session(session_id):
            raise HTTPException(404, "Session not found")
        return {"ok": True}

    @app.post("/v1/sessions/{session_id}/context")
    async def set_context(session_id: str, payload: ContextPatch) -> dict[str, Any]:
        session = await runtime.store.get_session(session_id)
        if not session:
            raise HTTPException(404, "Session not found")
        session.game_context = payload.game_context
        if payload.character_state is not None:
            session.character_state = payload.character_state
        session = await runtime.store.save_session(session)
        return json.loads(session.model_dump_json())

    @app.post("/v1/sessions/{session_id}/clear")
    async def clear_session(session_id: str) -> dict[str, Any]:
        if runtime.is_busy(session_id):
            raise HTTPException(409, "Interrupt the conversation before clearing history")
        session = await runtime.store.get_session(session_id)
        if not session:
            raise HTTPException(404, "Session not found")
        session.messages = []
        session = await runtime.store.save_session(session)
        return json.loads(session.model_dump_json())

    @app.post("/v1/sessions/{session_id}/turns")
    async def text_turn(session_id: str, payload: TextTurnRequest) -> dict[str, Any]:
        if runtime.is_busy(session_id):
            raise HTTPException(409, "Conversation is busy. Interrupt first.")
        events: list[dict[str, Any]] = []

        async def emit(message: LiveServerMessage) -> None:
            events.append(json.loads(message.model_dump_json()))

        session = await runtime.store.get_session(session_id)
        if not session:
            raise HTTPException(404, "Session not found")
        try:
            reply = await runtime.run_turn(session_id, payload.text, emit, source="text")
        except KeyError as exc:
            raise HTTPException(404, str(exc)) from exc
        except Exception as exc:
            raise HTTPException(500, str(exc)) from exc
        return {"reply": json.loads(reply.model_dump_json()), "events": events}

    @app.get("/v1/voices")
    async def voices() -> list[dict[str, Any]]:
        return runtime.speech.list_voices()

    @app.post("/v1/voices/preview")
    async def preview_voice(payload: VoicePreviewRequest) -> dict[str, Any]:
        try:
            return await runtime.preview_voice(payload.voice_id, payload.text, payload.speed)
        except Exception as exc:
            raise HTTPException(500, f"Voice preview failed: {exc}") from exc

    @app.get("/v1/providers")
    async def providers() -> list[dict[str, Any]]:
        return await runtime.inference.probe_all()

    @app.get("/v1/models")
    async def recommended_models() -> dict[str, Any]:
        from localtalker.catalog import recommended_status
        from localtalker.install import job_status

        return {"recommended": recommended_status(), "job": job_status()}

    @app.post("/v1/models/install")
    async def install_model(payload: ModelInstallRequest) -> dict[str, Any]:
        try:
            result = await runtime.activate_recommended(payload.id)
        except KeyError as exc:
            raise HTTPException(404, str(exc)) from exc
        except Exception as exc:
            raise HTTPException(500, str(exc)) from exc
        return result

    @app.websocket("/v1/sessions/{session_id}/live")
    async def live(websocket: WebSocket, session_id: str) -> None:
        origin = websocket.headers.get("origin")
        allowed = {f"http://{websocket.headers.get('host')}", "http://127.0.0.1:5173", "http://localhost:5173"}
        if origin and origin not in allowed:
            await websocket.close(code=4403)
            return
        session = await runtime.store.get_session(session_id)
        if not session:
            await websocket.close(code=4404)
            return
        await websocket.accept()
        send_lock = asyncio.Lock()
        work: asyncio.Task | None = None

        async def emit(message: LiveServerMessage) -> None:
            async with send_lock:
                await websocket.send_text(message.model_dump_json())

        async def dispatch(payload: dict[str, Any]) -> None:
            try:
                await runtime.handle_live(session_id, payload, emit)
            except asyncio.CancelledError:
                await emit(LiveServerMessage(type="cancelled"))
                await emit(LiveServerMessage(type="state", state="idle"))
            except Exception as exc:
                log.exception("Live turn failed")
                await emit(LiveServerMessage(type="error", error=str(exc)))
                await emit(LiveServerMessage(type="state", state="idle"))

        await emit(LiveServerMessage(type="ready", extra={"session_id": session_id}))
        try:
            while True:
                data = await websocket.receive_text()
                try:
                    payload = LiveClientMessage.model_validate_json(data).model_dump(exclude_none=True)
                except ValidationError as exc:
                    await emit(LiveServerMessage(type="error", error=f"Invalid message: {exc}"))
                    continue
                if payload["type"] == "interrupt":
                    if work and not work.done():
                        work.cancel()
                        await asyncio.gather(work, return_exceptions=True)
                    else:
                        await emit(LiveServerMessage(type="cancelled"))
                    runtime._audio_buffers.pop(session_id, None)
                    runtime._audio_meta.pop(session_id, None)
                    await emit(LiveServerMessage(type="state", state="idle"))
                elif payload["type"] in {"text", "end_audio"}:
                    if work and not work.done() or runtime.is_busy(session_id):
                        await emit(LiveServerMessage(type="error", error="Conversation is busy. Interrupt first."))
                    else:
                        work = asyncio.create_task(dispatch(payload))
                else:
                    await dispatch(payload)
        except WebSocketDisconnect:
            pass
        finally:
            if work and not work.done():
                work.cancel()
                await asyncio.gather(work, return_exceptions=True)
            runtime._audio_buffers.pop(session_id, None)
            runtime._audio_meta.pop(session_id, None)

    @app.get("/v1/scenes")
    async def list_scenes():
        return [scene.snapshot() for scene in runtime.scenes.scenes.values()]

    @app.post("/v1/scenes", status_code=201)
    async def create_scene(definition: SceneDefinition):
        return (await runtime.scenes.create(definition)).snapshot()

    @app.get("/v1/scenes/{scene_id}")
    async def get_scene(scene_id: str):
        scene = runtime.scenes.scenes.get(scene_id)
        if not scene: raise HTTPException(404, "Scene not found")
        return scene.snapshot()

    @app.delete("/v1/scenes/{scene_id}")
    async def delete_scene(scene_id: str):
        if not await runtime.scenes.delete(scene_id):
            raise HTTPException(404, "Scene not found")
        return {"ok": True}

    @app.get("/v1/groups")
    async def list_groups():
        return [scene.snapshot() for scene in runtime.scenes.scenes.values() if scene.kind == "group"]

    @app.post("/v1/groups", status_code=201)
    async def create_group(payload: GroupCreate):
        try:
            scene = await runtime.scenes.create_from_characters(
                payload.character_ids, payload.title, payload.player_name
            )
        except ValueError as exc:
            raise HTTPException(422, str(exc)) from exc
        return scene.snapshot()

    @app.get("/v1/groups/{scene_id}")
    async def get_group(scene_id: str):
        scene = runtime.scenes.scenes.get(scene_id)
        if not scene or scene.kind != "group":
            raise HTTPException(404, "Group chat not found")
        return scene.snapshot()

    @app.delete("/v1/groups/{scene_id}")
    async def delete_group(scene_id: str):
        scene = runtime.scenes.scenes.get(scene_id)
        if not scene or scene.kind != "group":
            raise HTTPException(404, "Group chat not found")
        await runtime.scenes.delete(scene_id)
        return {"ok": True}

    @app.post("/v1/scenes/{scene_id}/commands")
    async def scene_command(scene_id: str, command: SceneCommand):
        scene = runtime.scenes.scenes.get(scene_id)
        if not scene: raise HTTPException(404, "Scene not found")
        try: await scene.command(command)
        except ValueError as exc: raise HTTPException(422, str(exc)) from exc
        return scene.snapshot()

    @app.put("/v1/scenes/{scene_id}/definition")
    async def update_scene_definition(scene_id: str, definition: SceneDefinition):
        scene = runtime.scenes.scenes.get(scene_id)
        if not scene: raise HTTPException(404, "Scene not found")
        try: await scene.update_definition(definition)
        except ValueError as exc: raise HTTPException(422, str(exc)) from exc
        return scene.snapshot()

    async def bind_scene_socket(websocket: WebSocket, scene_id: str, require_group: bool = False) -> None:
        scene = runtime.scenes.scenes.get(scene_id)
        if not scene or (require_group and scene.kind != "group"):
            await websocket.close(code=4404)
            return
        origin = websocket.headers.get("origin")
        native_client = websocket.headers.get("x-localtalker-client") == "unreal"
        if origin and not native_client and origin not in {f"http://{websocket.headers.get('host')}", "http://127.0.0.1:5173", "http://localhost:5173"}:
            await websocket.close(code=4403)
            return
        await websocket.accept()
        queue = asyncio.Queue(maxsize=1024)
        scene.subscribers.add(queue)
        await websocket.send_json({"type": "scene", "scene": scene.snapshot()})
        async def sender():
            while True:
                await websocket.send_json(await queue.get())
        task = asyncio.create_task(sender())
        try:
            while True:
                raw = await websocket.receive_text()
                try: await scene.command(SceneCommand.model_validate_json(raw))
                except (ValueError, ValidationError) as exc:
                    await queue.put({"type": "error", "error": str(exc)})
        except WebSocketDisconnect:
            pass
        finally:
            scene.subscribers.discard(queue)
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
            if not scene.subscribers: await scene.stop()

    @app.websocket("/v1/scenes/{scene_id}/live")
    async def scene_live(websocket: WebSocket, scene_id: str):
        await bind_scene_socket(websocket, scene_id)

    @app.websocket("/v1/groups/{scene_id}/live")
    async def group_live(websocket: WebSocket, scene_id: str):
        await bind_scene_socket(websocket, scene_id, require_group=True)

    if static_dir and static_dir.exists():
        assets = static_dir / "assets"
        if assets.exists():
            app.mount("/assets", StaticFiles(directory=assets), name="assets")

        @app.get("/")
        async def index() -> FileResponse:
            return FileResponse(static_dir / "index.html")

        @app.get("/{path:path}")
        async def spa(path: str) -> FileResponse:
            if path.startswith("v1/"):
                raise HTTPException(404, "Unknown API endpoint")
            candidate = (static_dir / path).resolve()
            if candidate.is_relative_to(static_dir.resolve()) and candidate.is_file():
                return FileResponse(candidate)
            return FileResponse(static_dir / "index.html")

    return app


def build_runtime(cfg: AppConfig, db_file: Path) -> ConversationRuntime:
    return ConversationRuntime(cfg, Store(db_file))
