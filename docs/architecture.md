# Architecture

PersonalityCore is a character interaction runtime for human-authored games, with a standalone studio on top. Authors supply the script, character and canonical facts; dynamic replies and validated player requests extend that authored experience. See [the authored-character workflow](authored-characters.md).

```
Microphone / text
        │
        ▼
┌───────────────────┐     REST + WebSocket      ┌────────────────────┐
│ Developer studio  │ ─────────────────────────▶│ PersonalityCore│
│ (Electron / web)  │                           │ FastAPI + asyncio  │
└───────────────────┘                           └─────────┬──────────┘
                                                          │
                    ┌──────────────┬──────────────────────┼──────────────┐
                    ▼              ▼                      ▼              ▼
              Characters      Inference hub            Speech         SQLite
              & sessions      (OpenAI-compat,          faster-whisper  history
                              Ollama, llama.cpp,       Kokoro ONNX
                              mock)
```

Unreal and other engine clients use the same `/v1` interface. They do not embed the conversation loop. Unity, Godot, C++, C#, Python, and JavaScript clients can implement the same contract.

## Why this split

An earlier in-process llama.cpp integration inside Unreal fought GPU backend loading and versioned native libraries. Inference is therefore a **provider**, not a baked-in engine. The runtime speaks OpenAI-compatible HTTP so Ollama, llama.cpp `llama-server`, LM Studio, and vLLM are interchangeable.

Speech stays in-process (faster-whisper, kokoro-onnx) because those libraries already solve transcription and synthesis well on Windows.

## Core objects

- **Character** — name, personality, instructions, optional model/provider, voice, color
- **Session** — one conversation with history, free-form `game_context`, and `character_state`
- **CharacterReply** — `dialogue` plus structured emotion, intent, actions, animation, world interactions, tool requests, state changes
- **InferenceProvider** — how a character reaches a local (or compatible) model

There is no assumption of a single loaded model or a single speaking character. The studio talks to one session at a time; the protocol can host many.

## Turn path

1. Text arrives, or PCM16 audio is transcribed.
2. The user line is persisted.
3. The runtime builds a system prompt from character + game context + state + JSON schema.
4. Tokens stream over WebSocket (`token`, then incremental `dialogue`).
5. Completed sentences go to Kokoro as they appear.
6. Audio chunks stream back as base64 PCM16.
7. The full model output is validated into `CharacterReply` and stored.

Interruption sets a cancel flag, stops generation, and stops playback.

## Data

Default home: `%LOCALAPPDATA%\LocalTalker\`

- `localtalker.sqlite3` — characters and sessions
- `config.json` — providers and speech settings
- `speech/` — Whisper / Kokoro caches
- `models/` — optional GGUF files

Override with `LOCALTALKER_HOME`.

## Session isolation and desktop distribution

Each session has an independent generation task and cancellation event. The WebSocket read loop continues receiving control messages while a turn runs. One session cannot cancel another, and concurrent turns in the same session are rejected. Completed turns reload current host context before committing state.

The desktop schedules playback independently from model generation and keeps Speaking visible until the audio queue drains. Disconnect and character changes stop playback and capture.

Windows distribution uses [PyInstaller](https://pyinstaller.org/en/stable/runtime-information.html) for the runtime and the [Electron application layout](https://github.com/electron/electron/blob/main/docs/tutorial/application-distribution.md) for the desktop. `scripts/package.ps1` produces an unsigned portable folder with bundled Python, UI, and speech libraries. Models and llama.cpp remain separately configurable assets.
