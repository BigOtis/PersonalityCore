# LocalTalker In-Process Install Guide (Win64)

This guide is for projects using the in-process llama.cpp backend (`libllama.dll`) with Piper TTS.

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
- `Plugins/LocalTalker/ThirdParty/piper/Win64/Release/piper.exe`
- A `.gguf` model in `Plugins/LocalTalker/Resources/Models/`
- A voice `.onnx` in `Plugins/LocalTalker/Resources/Voices/`

Optional GPU backends:

- Vulkan: `ggml-vulkan.dll`
- CUDA: `ggml-cuda.dll`

If assets are missing, run:

```powershell
.\Tools\PrepareLocalTalkerBundle.ps1
```

## 3) Configure Project Settings

Open `Edit -> Project Settings -> LocalTalker`.

- Set `DefaultPaths` if your binaries/models are in custom locations.
- Set `BundledModelFile` to a model in `Resources/Models`.
- Configure `DefaultCharacterConfig` for generation behavior.
- Configure conversation gating/keep-alive under `Conversation`.

## 4) Hook up gameplay

- Add `LocalCharacterComponent` to NPC actors.
- Add `LocalPlayerInteractionComponent` to player actor.
- Trigger `SpeakToNearestAI("Hello")` or call `SendPromptAndSpeakStreamingInProc(...)` directly on NPCs.

## 5) Run tests

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

Local dependency smoke test:

```powershell
.\Tools\TestLocalTalker.ps1
```

## Troubleshooting

- `Failed to load libllama.dll`:
  - Check `DefaultPaths.LlamaLibPath`
  - Ensure all required `ggml*.dll` dependencies are present
- `Failed to load model`:
  - Check `DefaultPaths.LlamaModelPath` or `BundledModelFile`
  - Ensure the `.gguf` is complete and valid
- `piper failed`:
  - Check `piper.exe` and voice `.onnx` paths
- No AI reply:
  - Verify listener gating settings and actor proximity
