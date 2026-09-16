# Engine-neutral conversations

`src/localtalker/scenes.py` owns shared history, deterministic speaker scheduling, participation and conversation direction. Engine plugins own presentation, microphone capture, proximity and playback. Scene definitions use the existing character/voice schema and are persisted in SQLite along with the shared dialogue.

Create with `POST /v1/scenes` and a definition such as [`examples/conversation.json`](../examples/conversation.json). List with `GET /v1/scenes`, inspect with `GET /v1/scenes/{id}`. Connect to `ws://127.0.0.1:8765/v1/scenes/{id}/live`. The first event is `{ "type": "scene", "scene": ... }`. Commands can also be posted to `/v1/scenes/{id}/commands`.

Each member can supply `public_description`, `backstory`, and `relationships` keyed by other member IDs. Everyone sees the public descriptions; each character only receives their own private backstory and relationships. Their earlier speech is sent as their own assistant-role history, and other participants' speech is explicitly labeled. Director instructions are marked unspoken. This prevents a character from treating its own prior line as somebody else's instruction or replying to the director as an invisible player.

Revise a persisted cast with `PUT /v1/scenes/{id}/definition` and the complete definition. Member keys must remain unchanged; the route updates character prompts and scene briefs while preserving history. Create a new scene for a different cast or a fresh narrative experiment. JSON Schemas are in `protocol/scene-definition.v1.schema.json` and `protocol/scene-command.v1.schema.json`; regenerate with `scripts/export_scene_schemas.py`.

| Command | Fields |
| --- | --- |
| `presence` | `present`, `player_name` |
| `join`, `leave` | `player_name` |
| `text` | `text`, `target` (`all` or member key) |
| `audio` | `pcm16_b64`, `sample_rate`, `channels` |
| `end_audio` | `target` |
| `interrupt`, `pause` | none |
| `continue` | `turns` (1–30) |
| `direct` | optional `goal`, `guidance`, `context`, `join_policy` |
| `playback_done` | `turn_id` |
| `ping` | none |
| `speak` | `text`, `target`: host-authored voiced line |
| `reset` | Clear history and context, preserving cast and participation |

An open scene acknowledges and joins approaching players. An invitation scene requires an explicit join command. An ignore scene notices presence but excludes player contributions. Leaving updates the world context while droids may continue their discussion. Autonomous exchanges use a bounded round-robin roster. When a reply supplies a valid `addressee` member key, that peer's pending turn moves next so they can answer a direct question. Other pending turns are preserved. A specific player target replies alone, while `all` schedules every member once.

Each generated turn emits `turn_start`, incremental `dialogue`, PCM16 `audio`, structured `reply`, then `turn_end`. Events carry `scene_id`, `speaker` and `turn_id`. Route voice by member key. Discard audio from interrupted turns. After the final queued audio has played, send `playback_done` with the matching turn ID. The next character waits for that acknowledgement; a duration-based timeout lets a disconnected or silent host recover. `interrupted` means immediately clear playback. `scene_state` exposes idle/transcribing state; `participation` exposes joining state; `player_message` and `transcript` carry user speech/text.

`conversation_complete: true` proposes a natural pause. The runtime honors it after every invited character has spoken in that batch and no named peer is waiting to answer. Autonomous turn counts are ceilings. This lets an agreed plan end without filling the remaining budget with repeated agreement. Addressee promotion also avoids leaving a character with consecutive self-replies at the end of the roster.

Each droid receives the shared goal, guidance, participants, player presence and recent dialogue. Structured reply fields include emotion, animation, proposed actions and state changes. They are proposals for the host, not automatic engine execution. Per-character internal director prompts are reset each turn; the shared scene ledger preserves the conversation.

For Unity, implement the same WebSocket command/event loop, an AudioClip PCM queue with playback acknowledgement, and a proximity component. No model, speech or scheduling code needs to move into Unity.
