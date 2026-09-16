from __future__ import annotations

from datetime import datetime, timezone
from enum import Enum
from typing import Any, Literal
from uuid import uuid4

from pydantic import BaseModel, Field, StringConstraints
from typing import Annotated

CharacterName = Annotated[str, StringConstraints(strip_whitespace=True, min_length=1, max_length=100)]


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


def new_id(prefix: str) -> str:
    return f"{prefix}_{uuid4().hex[:12]}"


class ConversationState(str, Enum):
    idle = "idle"
    listening = "listening"
    transcribing = "transcribing"
    thinking = "thinking"
    speaking = "speaking"
    error = "error"


class ActionCue(BaseModel):
    name: str
    target: str | None = None
    parameters: dict[str, Any] = Field(default_factory=dict)


class CharacterReply(BaseModel):
    """Structured character output for hosts/engines.

    Dialogue is what the character says. Everything else is machine-readable
    intent for animation, gameplay, and tools — hosts should not parse dialogue.
    """

    dialogue: str = ""
    addressee: str | None = None
    conversation_complete: bool = False
    emotion: str | None = None
    intent: str | None = None
    actions: list[ActionCue] = Field(default_factory=list)
    animation: str | None = None
    world_interactions: list[ActionCue] = Field(default_factory=list)
    tool_requests: list[ActionCue] = Field(default_factory=list)
    state_changes: dict[str, Any] = Field(default_factory=dict)

    @classmethod
    def from_dialogue(cls, dialogue: str) -> CharacterReply:
        return cls(dialogue=dialogue.strip())


class VoiceSettings(BaseModel):
    voice_id: str = "af_bella"
    speed: float = Field(default=1.0, ge=0.5, le=2.0)
    language: str = "en-us"


class Character(BaseModel):
    id: str = Field(default_factory=lambda: new_id("char"))
    name: str
    personality: str = ""
    instructions: str = ""
    provider_id: str | None = None
    model: str | None = None
    # Overrides the provider's sampling temperature. A companion whose replies are
    # parsed into game actions wants a colder sampler than a theatrical NPC.
    temperature: float | None = Field(default=None, ge=0, le=2)
    voice: VoiceSettings = Field(default_factory=VoiceSettings)
    color: str = "#e2a35a"
    created_at: datetime = Field(default_factory=utcnow)
    updated_at: datetime = Field(default_factory=utcnow)


class CharacterCreate(BaseModel):
    name: CharacterName
    personality: str = ""
    instructions: str = ""
    provider_id: str | None = None
    model: str | None = None
    temperature: float | None = Field(default=None, ge=0, le=2)
    voice: VoiceSettings = Field(default_factory=VoiceSettings)
    color: str | None = None


class CharacterUpdate(BaseModel):
    name: CharacterName | None = None
    personality: str | None = None
    instructions: str | None = None
    provider_id: str | None = None
    model: str | None = None
    temperature: float | None = Field(default=None, ge=0, le=2)
    voice: VoiceSettings | None = None
    color: str | None = None


class ChatRole(str, Enum):
    system = "system"
    user = "user"
    assistant = "assistant"


class ChatMessage(BaseModel):
    id: str = Field(default_factory=lambda: new_id("msg"))
    role: ChatRole
    content: str
    reply: CharacterReply | None = None
    created_at: datetime = Field(default_factory=utcnow)


class Session(BaseModel):
    id: str = Field(default_factory=lambda: new_id("sess"))
    character_id: str
    title: str = "Conversation"
    messages: list[ChatMessage] = Field(default_factory=list)
    game_context: dict[str, Any] = Field(default_factory=dict)
    character_state: dict[str, Any] = Field(default_factory=dict)
    created_at: datetime = Field(default_factory=utcnow)
    updated_at: datetime = Field(default_factory=utcnow)


class SessionCreate(BaseModel):
    character_id: str
    title: str | None = None
    game_context: dict[str, Any] = Field(default_factory=dict)


class InferenceProviderKind(str, Enum):
    mock = "mock"
    openai_compat = "openai_compat"
    ollama = "ollama"
    llama_cpp = "llama_cpp"


class InferenceProvider(BaseModel):
    id: str
    name: str
    kind: InferenceProviderKind
    base_url: str = ""
    api_key: str = "local"
    model: str = ""
    temperature: float = Field(default=0.7, ge=0, le=2)
    max_tokens: int = Field(default=400, ge=16, le=8192)
    enabled: bool = True
    extra: dict[str, Any] = Field(default_factory=dict)


class TimingEvent(BaseModel):
    name: str
    at_ms: float
    detail: str | None = None


class TimingReport(BaseModel):
    started_at_ms: float
    events: list[TimingEvent] = Field(default_factory=list)
    totals_ms: dict[str, float] = Field(default_factory=dict)


class RuntimeStatus(BaseModel):
    ready: bool
    version: str
    state: ConversationState = ConversationState.idle
    active_session_id: str | None = None
    active_character_id: str | None = None
    providers: list[InferenceProvider]
    speech: dict[str, Any] = Field(default_factory=dict)
    gpu: dict[str, Any] = Field(default_factory=dict)
    errors: list[str] = Field(default_factory=list)


class VoiceInfo(BaseModel):
    id: str
    name: str
    language: str = "en-us"
    gender: str | None = None
    traits: str | None = None


class LiveClientMessage(BaseModel):
    type: Literal[
        "text",
        "audio",
        "end_audio",
        "interrupt",
        "set_context",
        "ping",
    ]
    text: str | None = Field(default=None, max_length=32000)
    pcm16_b64: str | None = Field(default=None, max_length=11000000)
    sample_rate: int | None = Field(default=None, ge=8000, le=192000)
    channels: int = Field(default=1, ge=1, le=2)
    game_context: dict[str, Any] | None = None
    listen_mode: Literal["push_to_talk", "auto"] | None = None


class LiveServerMessage(BaseModel):
    type: str
    state: ConversationState | None = None
    text: str | None = None
    delta: str | None = None
    reply: CharacterReply | None = None
    timing: TimingReport | None = None
    error: str | None = None
    pcm16_b64: str | None = None
    sample_rate: int | None = None
    chunk_index: int | None = None
    voice_id: str | None = None
    transcript: str | None = None
    message_id: str | None = None
    extra: dict[str, Any] = Field(default_factory=dict)
