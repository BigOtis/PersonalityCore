# Whisper STT Worker

This folder contains the LocalTalker Whisper speech-to-text worker.

## Install dependencies

```bash
pip install -r requirements-whisper-stt.txt
```

## Runtime

`whisper_stt_worker.py` is launched by `ULocalPlayerInteractionComponent` as a long-lived subprocess and communicates via JSON-lines over stdin/stdout.
