# LocalTalker (Win64, in-process llama.cpp + Qwen3-TTS)

LocalTalker is a Blueprint-first Unreal plugin for local character conversations:

- Local LLM generation with llama.cpp (in-process, streaming token deltas)
- Local TTS with Qwen3-TTS via a persistent Python worker process
- Conversation Director subsystem (turn-taking, proximity grouping, auto replies)
- Player interaction component for routing player text to nearby AI actors

Tested in this repo with Unreal Engine 5.7 on Win64.

## Quick start (this repo)

1. Open `AutoChat.uproject` (plugin is already enabled).
2. Verify core files:
   - `Plugins/LocalTalker/ThirdParty/llama/Win64/Release/libllama.dll`
   - `Plugins/LocalTalker/Resources/Models/Llama-3.2-3B-Instruct-Q6_K_L.gguf`
   - `Plugins/LocalTalker/Resources/Qwen/qwen_tts_worker.py`
3. Install Python dependencies in your runtime environment:
   - `pip install -r Plugins/LocalTalker/Resources/Qwen/requirements-qwen-tts.txt`
4. In editor, open `Edit -> Project Settings -> LocalTalker` and set:
   - `TTS.Backend = Qwen3-TTS Worker`
   - `DefaultPaths.QwenPythonExePath` (if `python` on PATH is not sufficient)
   - `DefaultPaths.QwenModelPath`
   - `DefaultPaths.QwenTokenizerPath`
   - `DefaultPaths.QwenDevice` / `DefaultPaths.QwenDType`
5. Add `LocalCharacterComponent` to NPC actors.
6. Add `LocalPlayerInteractionComponent` to the player actor and call `SpeakToNearestAI(...)`.

## Bring-your-own voices

The plugin does not create voice clones in-editor. You can bring your own by:

1. Generating voice prompt assets externally (for Qwen Base model) and including them in your project.
2. Adding entries under `Project Settings -> LocalTalker -> Voices`:
   - `Id` (dropdown/display key)
   - `QwenSpeaker` (for CustomVoice models)
   - `QwenInstruction` (optional style instruction)
   - `QwenVoicePromptPath` (optional prompt file path for Base model)
3. Selecting `VoiceId` on each `LocalCharacterComponent`.

Supported Qwen model modes in plugin runtime:
- `custom_voice` (speaker IDs from model)
- `base` (externally generated prompt assets)
- `voice_design` is intentionally not supported.

## Runtime path behavior

`ULocalCharacterComponent::ResolvePaths()` resolves files in this order:

1. Project settings (`DefaultPaths`)
2. Per-component `PathsOverride` values (when set)
3. Plugin defaults (if still empty), including:
   - `ThirdParty/llama/Win64/Release/libllama.dll`
   - `Resources/Models/<BundledModelFile>` when set and found
   - fallback model: `Resources/Models/Llama-3.2-3B-Instruct-Q6_K_L.gguf`
   - `Resources/Qwen/qwen_tts_worker.py`
   - default Qwen model/tokenizer ids

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

- TTS:
  - `TtsBackend`
  - `bQwenUseFlashAttention`
  - `QwenRequestTimeoutSeconds`
- Paths:
  - `DefaultPaths`
  - `BundledModelFile` (dropdown from `Resources/Models/*.gguf`)
- Voices:
  - `Voices` array (`FLocalTalkVoiceOption`) for speaker/prompt selection
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

## Troubleshooting

- "Qwen worker script not found"
  - Check `DefaultPaths.QwenWorkerScriptPath`
- "Failed to start Qwen worker"
  - Check `DefaultPaths.QwenPythonExePath`
  - Ensure Python env has `qwen-tts` and dependencies installed
- "Qwen synthesis failed"
  - Verify selected `QwenSpeaker` is valid for the loaded model
  - For Base model, provide `QwenVoicePromptPath` generated externally
- "Failed to load libllama.dll"
  - Check `DefaultPaths.LlamaLibPath`
  - Ensure `ggml*.dll` deps are present next to `libllama.dll`
