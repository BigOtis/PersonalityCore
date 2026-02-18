# LocalTalker (Win64, in-process llama.cpp + Piper)

LocalTalker is a Blueprint-first Unreal plugin for local character conversations:

- Local LLM generation with llama.cpp (in-process, streaming token deltas)
- Local TTS with Piper (sentence-based WAV generation, procedural playback)
- Conversation Director subsystem (turn-taking, proximity grouping, auto replies)
- Player interaction component for routing player text to nearby AI actors

Tested in this repo with Unreal Engine 5.7 on Win64.

## Quick start (this repo)

1. Open `AutoChat.uproject` (plugin is already enabled).
2. Verify required runtime files exist:
   - `Plugins/LocalTalker/ThirdParty/llama/Win64/Release/libllama.dll`
   - `Plugins/LocalTalker/ThirdParty/piper/Win64/Release/piper.exe`
   - `Plugins/LocalTalker/Resources/Models/Llama-3.2-3B-Instruct-Q6_K_L.gguf`
   - `Plugins/LocalTalker/Resources/Voices/en_US-lessac-small.onnx`
3. In editor, open `Edit -> Project Settings -> LocalTalker`.
4. Add `LocalCharacterComponent` to NPC actors.
5. Add `LocalPlayerInteractionComponent` to the player actor and call `SpeakToNearestAI(...)`.

## Install into another project

1. Copy `Plugins/LocalTalker` into your target project's `Plugins/` folder.
2. Enable `LocalTalker` in `Edit -> Plugins`.
3. Restart editor when prompted.
4. Configure runtime paths in `Project Settings -> LocalTalker` if your files are in non-default locations.

If binaries or assets are missing, run:

```powershell
.\Tools\PrepareLocalTalkerBundle.ps1
```

## Runtime path behavior

`ULocalCharacterComponent::ResolvePaths()` resolves files in this order:

1. Project settings (`DefaultPaths`)
2. Per-component `PathsOverride` values (when set)
3. Plugin defaults (if still empty), including:
   - `ThirdParty/llama/Win64/Release/libllama.dll`
   - `ThirdParty/piper/Win64/Release/piper.exe`
   - `Resources/Voices/en_US-lessac-small.onnx`
   - `Resources/Models/<BundledModelFile>` when set and found
   - fallback model: `Resources/Models/Llama-3.2-3B-Instruct-Q6_K_L.gguf`

## Main runtime pieces

- `ULocalCharacterComponent` (`DisplayName="LocalTalk"`)
  - `SendPromptAndSpeakStreamingInProc(Prompt)`
  - `SpeakTextLocal(Text)`
  - `Interrupt()`
  - Emits `OnToken`, `OnSpokenText`, `OnSubtitle`, `OnError`
- `ULocalPlayerInteractionComponent` (`DisplayName="LocalTalk Player Interaction"`)
  - `FindNearestAI(MaxRange)`
  - `SpeakToAI(TargetAI, PlayerText)`
  - `SpeakToNearestAI(PlayerText, MaxRange)`
- `ULocalTalkConversationSubsystem`
  - Registers talkers, maintains proximity contexts, manages turns and auto replies

## Key settings to know

Project settings: `Edit -> Project Settings -> LocalTalker`

- Paths:
  - `DefaultPaths`
  - `BundledModelFile` (dropdown from `Resources/Models/*.gguf`)
- Character defaults:
  - `DefaultCharacterConfig` (sampling, max tokens, gpu backend/layers, stop sequences)
- Conversation controls:
  - `bAllowNpcToNpcAuto`
  - `MaxConsecutiveNpcTurns`
  - `bRequirePlayerListenerForAuto`
  - `bRequirePlayerListenerForAllTalk`
  - `MinSecondsBetweenAutoReplies`
  - `bKeepConversationAlive`
  - `MaxSilenceSeconds`
  - `bKeepAliveIgnoresPlayerListenerRequirement`
  - `ContextCleanupSeconds`
- Performance controls:
  - `AutoGpuLayerCap`
  - `MinContextTokens`
  - `MaxContextTokens`
  - `ContextTokenMargin`
- Voices:
  - `Voices` array (`FLocalTalkVoiceOption`)
- Microphone defaults:
  - `MicInputDeviceMode`
  - `MicInputDeviceName`

Per-component overrides are available on `ULocalCharacterComponent`:
- Paths/config overrides
- `VoiceId`
- Streaming chunking controls (`bSpeakStreaming`, `MinCharsBeforeSpeak`, phrase/word chunk settings)
- Subtitle and debug options

## Automation tests

Conversation E2E tests currently include:

- `Plugins.LocalTalker.Dialog.E2E.SpawnedTwoCharacterConversation`
- `Plugins.LocalTalker.Dialog.E2E.PlayerInputReaction`
- `Plugins.LocalTalker.Dialog.E2E.ContextIsolationByDistance`
- `Plugins.LocalTalker.Dialog.E2E.PlayerInteractionComponentRoutesToAI`

Run from command line:

```powershell
& "C:\Program Files\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "C:\Path\To\Project.uproject" `
  -NullRHI -Unattended -NoSplash -NoPause -NoSound `
  -ExecCmds="Automation RunTests Plugins.LocalTalker.Dialog.E2E;Quit"
```

Or use packaged-plugin test runner:

```powershell
.\Tools\RunPackagedLocalTalkerTests.ps1 `
  -EngineRoot "C:\Program Files\Epic Games\UE_5.7" `
  -TestFilter "Plugins.LocalTalker.Dialog.E2E"
```

For local file/binary smoke checks:

```powershell
.\Tools\TestLocalTalker.ps1
```

## GPU notes

- GPU offload is controlled by `GpuBackend` and `GpuLayers`.
- `GpuLayers=0` uses auto mode and applies `AutoGpuLayerCap` when GPU backend is available.
- Vulkan requires `ggml-vulkan.dll` next to `libllama.dll`.
- CUDA requires `ggml-cuda.dll` next to `libllama.dll`.
- If backend DLLs are present but offload is unavailable, LocalTalker falls back to CPU and logs a warning.

## Troubleshooting

- "Failed to load libllama.dll"
  - Check `DefaultPaths.LlamaLibPath`
  - Ensure dependent DLLs are next to `libllama.dll` (`ggml*.dll`, runtime deps)
- "Failed to load model"
  - Check `DefaultPaths.LlamaModelPath` or `BundledModelFile`
  - Verify `.gguf` is valid and not truncated
- "piper failed ..."
  - Ensure `piper.exe` and its runtime DLLs exist
  - Ensure selected `.onnx` voice exists
- No one responds
  - Check listener gating settings (`bRequirePlayerListenerForAuto`, `bRequirePlayerListenerForAllTalk`)
  - Check `ConversationRadius` / attenuation radius overlap
  - Enable trace with `LocalTalker.TraceConversation=1`
