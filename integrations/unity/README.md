# Unity integration path

LocalTalker's runtime and protocol are engine-independent. A packaged Unity adapter is not included yet. A Unity client can use `System.Net.Http.HttpClient` and `System.Net.WebSockets.ClientWebSocket` on supported desktop targets.

The prescribed host boundary is:

1. Create characters and sessions with REST, or POST a multi-character definition to `/v1/scenes`.
2. Connect to `/v1/scenes/{id}/live` and join the scene.
3. Send text, or microphone samples converted to mono signed little-endian PCM16 with the actual sample rate.
4. Route incoming audio by `speaker`; convert PCM16 to floats for Unity playback. Queue chunks in order and correlate them with `turn_id`.
5. Send `playback_done` only after the final chunk for that scene turn has played. On `interrupted`, stop and discard that turn's buffered audio.
6. Marshal Unity object access onto the main thread. Validate structured actions, execute supported actions, and send the resulting world context back to the runtime.

Keep inference, transcription, synthesis, and conversation scheduling in the runtime process. Keep actors, AudioSources, animation, permissions, and gameplay state in Unity. Mobile, WebGL, console deployment, and multiplayer authority need separate platform work and are not claimed as supported by this release.

See [the engine contract](../../docs/integration.md), [scenes](../../docs/scenes.md), and [JSON schemas](../../protocol).
