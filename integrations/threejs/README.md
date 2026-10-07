# PersonalityCore / Three.js

A small browser client for the independent PersonalityCore runtime (formerly LocalTalker): character
selection, typed conversation, streamed voice, interruption and a Three.js
character scene. It uses your existing studio characters and configured model.
It needs neither Unreal nor Cohersion.

## Run

Start the runtime from the LocalTalker repository root:

```powershell
.\.venv\Scripts\python.exe -m localtalker serve
```

Then, in another terminal:

```powershell
cd integrations/threejs
npm install
npm run dev
```

Open `http://127.0.0.1:5173`, choose a character and click **Connect**. Type a
message and click **Send**. **Interrupt** cancels the current turn and clears
the queued sound. Model/provider setup still lives in LocalTalker's studio.

The Vite development proxy forwards HTTP and WebSocket `/v1` requests to
`http://127.0.0.1:8765`. Set `LOCALTALKER_URL` before launching Vite to use a
different runtime. Do not run this example and the studio's Vite dev server
on port 5173 simultaneously. The built studio on port 8765 can remain running.

## What the browser owns

`main.js` creates a single-character session, opens its live socket, supplies
host facts and schedules PCM16 chunks in Web Audio. Audio is enabled by the
Connect gesture. A spatial audio node places the voice in front of the listener.
The rendered character bobs during queued voice playback. The example displays
structured action proposals without executing them; its host context offers no
gameplay actions.

When extending it, add a local allowlist of actions and resolve real object IDs
before moving anything. Report actual results through `set_context`; do not
drive gameplay by parsing dialogue. Supply your own animation rig, physics,
microphone capture or scene controls as needed. The [session API](../../docs/integration.md)
already accepts PCM16 for transcription; the studio demonstrates microphone
capture. The [scene API](../../docs/scenes.md) supports multiple speakers and
playback acknowledgements if your browser scene needs a cast.

This example is typed-input integration, not a complete browser game or a
browser-local inference engine. It does not implement microphone recording,
autonomous multi-character scenes or a production action executor.

## Build and host

`npm run build` produces `dist/`. Vite's development proxy is not included in
that output. A deployed client needs an HTTP/WebSocket reverse proxy for `/v1`
to a runtime it can reach, or an explicit runtime URL with a deliberately
configured origin. Loopback refers to the browser user's machine. Keep the
runtime's existing local binding unless you intentionally design hosted access.

The example vendors neither game assets nor model weights. Three.js and Vite
versions are recorded in its package manifest/lockfile. See the official
[Three.js documentation](https://threejs.org/docs/) for rendering and audio APIs.
