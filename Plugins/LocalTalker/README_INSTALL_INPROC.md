# LocalTalker In-Process Install Guide (Win64, Qwen3-TTS)

This guide is for projects using in-process llama.cpp plus Qwen3-TTS worker synthesis.

## 1) Add plugin

1. Copy `Plugins/LocalTalker` into your project's `Plugins` folder.
2. Enable `LocalTalker` in `Edit -> Plugins`.
3. Restart Unreal Editor.

## 2) Verify required files

Minimum required runtime files:

- `Plugins/LocalTalker/ThirdParty/llama/Win64/Release/libllama.dll`
- `Plugins/LocalTalker/ThirdParty/llama/Win64/Release/ggml.dll`
- `Plugins/LocalTalker/ThirdParty/llama/Win64/Release/ggml-base.dll`
- `Plugins/LocalTalker/ThirdParty/llama/Win64/Release/ggml-cpu.dll`
- A `.gguf` model in `Plugins/LocalTalker/Resources/Models/`
- `Plugins/LocalTalker/Resources/Qwen/qwen_tts_worker.py`

## 3) Prepare Python runtime for Qwen TTS

Install dependencies in the Python environment you want Unreal to use:

```powershell
pip install -r Plugins/LocalTalker/Resources/Qwen/requirements-qwen-tts.txt
```

## 4) Configure Project Settings

Open `Edit -> Project Settings -> LocalTalker`.

- Set `TTS.Backend` to `Qwen3-TTS Worker`.
- Set `DefaultPaths.QwenPythonExePath` (if needed).
- Set `DefaultPaths.QwenModelPath` (repo id or local model dir).
- Set `DefaultPaths.QwenTokenizerPath` (repo id or local tokenizer dir).
- Optionally tune `DefaultPaths.QwenDevice` and `DefaultPaths.QwenDType`.
- Add voice entries under `Voices` (`QwenSpeaker` and/or `QwenVoicePromptPath`).
- Runtime supports `custom_voice` and `base` Qwen model modes; `voice_design` mode is intentionally unsupported.

## 5) Hook up gameplay

- Add `LocalCharacterComponent` to NPC actors.
- Add `LocalPlayerInteractionComponent` to player actor.
- Trigger `SpeakToNearestAI("Hello")` or call `SendPromptAndSpeakStreamingInProc(...)` directly on NPCs.

## 6) Run tests

Editor-cmd automation:

```powershell
& "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "C:\Path\To\Project.uproject" `
  -NullRHI -Unattended -NoSplash -NoPause -NoSound `
  -ExecCmds="Automation RunTests Plugins.LocalTalker.Dialog.E2E;Quit"
```

Packaged plugin test runner:

```powershell
.\Tools\RunPackagedLocalTalkerTests.ps1 `
  -EngineRoot "C:\Program Files\Epic Games\UE_5.7" `
  -TestFilter "Plugins.LocalTalker.Dialog.E2E"
```

## Troubleshooting

- `Qwen worker script not found`:
  - Check `DefaultPaths.QwenWorkerScriptPath`
- `Failed to start Qwen worker`:
  - Check `QwenPythonExePath`
  - Ensure Python env has Qwen dependencies installed
- `Qwen synthesis failed`:
  - Verify `QwenSpeaker` is valid for model
  - For Base model, ensure `QwenVoicePromptPath` points to an externally generated prompt asset
- `Failed to load model`:
  - Check `DefaultPaths.LlamaModelPath` or `BundledModelFile`
  - Ensure `.gguf` file is complete and valid
