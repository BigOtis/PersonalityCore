# Qwen3-TTS worker assets

This folder contains the LocalTalker worker script and Python dependency list for Qwen3-TTS.

- `qwen_tts_worker.py`: persistent stdin/stdout JSON worker used by LocalTalker runtime.
- `requirements-qwen-tts.txt`: baseline Python package dependencies.

Voice cloning prompt generation is intentionally out-of-scope for the plugin runtime.
Bring externally-generated prompt assets (for Base model inference) and reference them via:

`Project Settings -> LocalTalker -> Voices -> QwenVoicePromptPath`

`voice_design` model mode is intentionally unsupported in LocalTalker runtime.
