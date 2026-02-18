# Bundled voice notes

This folder contains Piper voice models used by LocalTalker.

Voice resolution behavior:

1. `ULocalCharacterComponent.VoiceId` (if set)
2. Matching entry in `Project Settings -> LocalTalker -> Voices`
3. First valid voice entry in `Voices`
4. Fallback: `Resources/Voices/en_US-lessac-small.onnx`

When adding voices:

1. Copy `.onnx` (and optional `.onnx.json`) into this folder.
2. Add/update `Voices` entries in project settings.
3. Set component `VoiceId` where needed.
