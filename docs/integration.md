# Host / engine integration

Do not move the conversation runtime into Unreal, Unity, or Godot. Write a thin client.

## Transport

- HTTP JSON on `http://127.0.0.1:8765`
- WebSocket on `/v1/sessions/{session_id}/live`

The studio is the reference client.

## Typical host sequence

1. `GET /v1/health` — runtime is up
2. `GET /v1/characters` or `POST /v1/characters`
3. `POST /v1/sessions` with `{ "character_id", "game_context" }`
4. Open the live WebSocket
5. Each frame or trigger, `POST /v1/sessions/{id}/context` or send `{ "type": "set_context", "game_context": { ... } }`
6. Player speech or subtitle → `{ "type": "text", "text": "..." }` or audio PCM16
7. Handle `token` / `dialogue` for subtitles, `audio` for voice, `reply` for gameplay

## Live client messages

| type | purpose |
| --- | --- |
| `text` | Player line |
| `audio` | Base64 PCM16 chunk (`sample_rate`, `channels`) |
| `end_audio` | Close the utterance and transcribe |
| `interrupt` | Barge-in |
| `set_context` | Replace `game_context` |
| `ping` | Liveness |

## Live server messages

| type | purpose |
| --- | --- |
| `ready` | Socket accepted |
| `state` | `idle` / `listening` / `transcribing` / `thinking` / `speaking` / `error` |
| `user_message` | Persisted player line |
| `transcript` | STT result |
| `token` | Raw model delta |
| `dialogue` | Incremental spoken text |
| `audio` | PCM16 chunk to play |
| `reply` | Final `CharacterReply` + timings |
| `error` / `warning` / `cancelled` | Recoverable failures |

## Structured reply

```json
{
  "dialogue": "The gate stays closed tonight.",
  "emotion": "stern",
  "intent": "refuse",
  "animation": "arms_cross",
  "actions": [{ "name": "block_path", "target": "gate" }],
  "world_interactions": [],
  "tool_requests": [],
  "state_changes": { "trust": -1 }
}
```

Drive animation and AI from these fields. Do not parse `dialogue` for gameplay.

## Game context

`game_context` is an arbitrary JSON object. Do not special-case an RPG schema in the runtime. Send location, inventory, nearby characters, time, or anything else the host knows.

## Language bindings

Any HTTP/WebSocket client is enough:

- C++: libcurl + a WebSocket library
- C#: `HttpClient` + `ClientWebSocket`
- Python: `httpx` / `websockets`
- JavaScript: `fetch` + `WebSocket`

Keep engine code to: start/find the runtime process, map actors to `character_id`, push world JSON, play returned audio, apply `actions`.
