# Testing

`python -m pytest -q` runs deterministic unit, integration, and API flow tests.
They cover persistence, character voice edits, structured validation and Unicode
streaming, PCM resampling, provider selection, muted synthesis, invalid requests,
parallel characters, and interruption while inference is stalled.

From `app`, run `npm run build` and `npm run test:e2e` to start an isolated runtime
on port 8876 and drive Chromium. Install its browser with `npx playwright install
chromium`. Tests type into the actual UI, observe streamed WebSocket events,
schedule real Web Audio buffers, reload persisted history, switch characters,
edit a character, inject context, and capture microphone samples through
AudioWorklet. Only inference and speech engines are deterministic doubles; no
routes or UI functions are mocked. This test never reads the user's database.

Hardware tests are opt-in. Start a configured runtime, then set
`LOCALTALKER_SMOKE=1` and run `python -m pytest -m smoke -q`.
`LOCALTALKER_URL` overrides the default `http://127.0.0.1:8765` for the full speech
loop. `LOCALTALKER_HOME` selects configuration for the direct inference smoke.
The direct provider test skips if no provider is reachable; the voice-loop test
fails if the running runtime is not configured for real inference.

`node app/desktop-smoke.cjs` checks a native Electron window. With
`LOCALTALKER_SMOKE=1`, it additionally runs real text and microphone turns using
`.localtalker/validation/mic-input.wav`, produced by the hardware speech test.
Set `SMOKE_EXE` to a packaged `LocalTalker.exe` and `SMOKE_PORT` to an unused port
to test bundled startup, speech, and shutdown. The test removes its temporary
character and closes the application.

Tests do not establish physical microphone acoustics or speaker sound quality.
Real-model tests require a separately configured local provider.
