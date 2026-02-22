# Bundled voice notes

This folder contains voice-related assets used by LocalTalker.

For Qwen3-TTS integration, common asset patterns are:

- externally-generated Base-model prompt files (example extension: `.voiceprompt.pt`)
- optional metadata files your project uses for organizing voice packs

Voice resolution behavior in LocalTalker:

1. `ULocalCharacterComponent.VoiceId` (if set)
2. matching entry in `Project Settings -> LocalTalker -> Voices`
3. first valid entry in `Voices`
4. optional auto-discovery by `VoiceId.voiceprompt.pt` in this folder

The plugin does not generate clone prompt assets. Create them externally and include them here (or provide absolute paths in settings).
