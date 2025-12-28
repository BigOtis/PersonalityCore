# LocalTalker (UE 5.5) — Local LLM + Local TTS (Streaming)

LocalTalker is a **Blueprint-first** UE plugin that lets you drive interactive characters using:

- **Local LLM (llama.cpp)** — in-process, streaming tokens with GPU acceleration
- **Local TTS (Piper)** — generates WAV audio and plays it via `USoundWaveProcedural`
- **Conversation Director** — manages turn-taking, proximity groups, and multi-character conversations

This repo currently focuses on **Win64 / UE 5.5**.

> Current state: LLM streaming is implemented in-process with Vulkan GPU acceleration support. TTS works via Piper CLI per sentence. The plugin includes a conversation subsystem that manages turn-taking and proximity-based conversation groups, enabling NPCs to respond to each other automatically.

---

## Architecture Overview

LocalTalker uses a **Director pattern** to orchestrate conversations:

1. **LocalCharacterComponent** - Individual characters that can speak and listen
2. **LocalTalkConversationSubsystem** - The "Director" that manages turn-taking and proximity groups
3. **Streaming Pipeline** - LLM generates tokens → sentences extracted → TTS converts to audio → plays via procedural audio

### How It Works

Characters register with the **Conversation Subsystem** on BeginPlay. When a character wants to speak, they request a turn. The Director:
- Groups characters by proximity (within each character's `ConversationRadius`)
- Manages turn-taking (only one speaker per conversation group at a time)
- Maintains conversation history per proximity group
- Can automatically trigger NPC responses to user messages or other NPCs
- Handles interruptions and cleanup

The LLM uses a specialized **prompt wrapper** that ensures characters output natural dialogue (not chat transcripts like "Assistant: ..."). Text is sanitized to remove speaker labels and control tokens, making it suitable for direct speech.

---

## GPU Acceleration (Vulkan)

LocalTalker supports **Vulkan GPU acceleration** for significantly faster LLM inference. When enabled, model layers are offloaded to your GPU.

### Requirements

- A Vulkan-capable GPU (most modern NVIDIA, AMD, and Intel GPUs)
- Vulkan runtime installed (usually included with GPU drivers)
- `ggml-vulkan.dll` present alongside `libllama.dll`

### Enabling Vulkan

1. Ensure your llama.cpp build includes Vulkan support:
   ```powershell
   cmake .. -DBUILD_SHARED_LIBS=ON -DGGML_VULKAN=ON
   cmake --build . --config Release
   ```

2. Copy the resulting DLLs to `ThirdParty/llama/Win64/Release/`:
   - `libllama.dll`
   - `ggml.dll`
   - `ggml-base.dll`
   - `ggml-cpu.dll`
   - `ggml-vulkan.dll` ← Required for GPU acceleration

3. The plugin automatically uses GPU offload when `ggml-vulkan.dll` is present.

### Verifying GPU Acceleration

Run the automation tests (see below) — they will report whether Vulkan backend is detected.

You can also check the Unreal log for:
```
llama.cpp reports GPU offload is supported
```

### CPU-Only Fallback

If `ggml-vulkan.dll` is not present or Vulkan is unavailable, the plugin falls back to CPU-only inference. This works but is significantly slower for larger models.

---

## Automation Tests

LocalTalker includes automated tests to verify your setup before runtime. **Run these tests to catch configuration issues early.**

### Running Tests

From command line (PowerShell):

```powershell
& "D:\Unreal\UE_5.5\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "C:\Path\To\YourProject.uproject" `
  -NullRHI -Unattended -NoSplash -NoPause -NoSound `
  -ExecCmds="Automation RunTests LocalTalker;Quit"
```

Or from the Unreal Editor:
1. Open **Window → Developer Tools → Session Frontend**
2. Go to the **Automation** tab
3. Filter for "LocalTalker"
4. Run all tests

### Available Tests

| Test | Description |
|------|-------------|
| `LocalTalker.RequiredFiles.Exist` | Verifies all required runtime files exist (DLLs, model, voice) |
| `LocalTalker.DLL.Load` | Confirms `libllama.dll` is present and accessible |
| `LocalTalker.Model.Validate` | Validates model file exists and has reasonable size (catches corrupt/incomplete downloads) |
| `LocalTalker.Conversation.Subsystem` | Verifies the conversation subsystem initializes correctly |

### Common Test Failures

**"Model file is too small"**
- Your `.gguf` file is likely a partial download or error page
- Delete it and re-download from HuggingFace:
  - [TinyLlama Q4_K_M](https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf) (~637 MB)

**"MISSING: libllama.dll"**
- Build llama.cpp or download a pre-built release
- Place in `ThirdParty/llama/Win64/Release/`

**"MISSING: piper.exe"**
- Download Piper from [rhasspy/piper releases](https://github.com/rhasspy/piper/releases)
- Extract to `ThirdParty/piper/Win64/Release/`

---

## Quick Start

### 1) Ensure the plugin is installed and enabled

This project already contains the plugin here:

- `Plugins/LocalTalker`

The sample project (`AutoChat.uproject`) already enables it. In another project:

1. Copy the folder `Plugins/LocalTalker` into your project's `Plugins/` directory
2. Open UE
3. Enable **LocalTalker** in **Edit → Plugins**
4. Restart the editor if prompted

### 2) Provide the bundled runtime artifacts (required)

LocalTalker expects these files to exist (default "out of the box" locations):

- **llama.cpp DLLs**
  - `Plugins/LocalTalker/ThirdParty/llama/Win64/Release/libllama.dll`
  - `Plugins/LocalTalker/ThirdParty/llama/Win64/Release/ggml.dll`
  - `Plugins/LocalTalker/ThirdParty/llama/Win64/Release/ggml-base.dll`
  - `Plugins/LocalTalker/ThirdParty/llama/Win64/Release/ggml-cpu.dll`
  - `Plugins/LocalTalker/ThirdParty/llama/Win64/Release/ggml-vulkan.dll` (optional, for GPU)
- **Default GGUF model**
  - `Plugins/LocalTalker/Resources/Models/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf`
- **Piper executable**
  - `Plugins/LocalTalker/ThirdParty/piper/Win64/Release/piper.exe`
- **Default voice model**
  - `Plugins/LocalTalker/Resources/Voices/en_US-lessac-small.onnx`
  - `Plugins/LocalTalker/Resources/Voices/en_US-lessac-small.onnx.json`

**Download the model:**
- [TinyLlama Q4_K_M](https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf) (~637 MB)

### 3) Configure Project Settings (optional)

Open:

- **Edit → Project Settings → LocalTalker**

Set paths and defaults:

- `DefaultPaths` - Runtime file paths (DLLs, model, piper, voice)
- `DefaultCharacterConfig` - LLM parameters (temperature, max tokens, GPU layers, etc.)
- `Voices` - Available voice options (for component dropdowns)
- `bAllowNpcToNpcAuto` - Allow NPCs to automatically respond to each other
- `MaxConsecutiveNpcTurns` - Cap on NPC-to-NPC conversation loops (0 = unlimited)
- `bRequirePlayerListenerForAuto` - Only auto-respond if a player is nearby
- `MinSecondsBetweenAutoReplies` - Pacing control for automatic responses

Per-character overrides are available on the component itself.

### 4) Add a character component and test

1. Create an `Actor` (or use an existing actor)
2. Add **LocalCharacterComponent** (display name: "LocalTalk")
3. Configure the character:
   - `SpeakerName` - Name shown in subtitles (defaults to actor label)
   - `ConversationRadius` - How far this character can "hear" others (default: 1500)
   - `Directions` - Character behavior/personality instructions
   - `Desc` - Character description
   - `PathsOverride` - Override runtime paths (if not using project settings)
   - `CharacterConfigOverride` - Override LLM parameters
4. On BeginPlay or via Blueprint event, call:
   - `SendPromptAndSpeakStreamingInProc("Hello!")` - Generate dialogue via LLM
   - `SpeakTextLocal("Hello!")` - Speak raw text (skip LLM)

What you should see/hear:

- **Streaming text**: `OnToken` fires as tokens arrive
- **Streaming speech**: TTS begins once sentence chunks are extracted (punctuation or flush)
- **Subtitles**: `OnSubtitle` fires with speaker name and text
- **Turn management**: Only one character speaks at a time per conversation group

---

## Blueprint API

### `ULocalCharacterComponent` (Display Name: "LocalTalk")

#### Methods

- **`SendPromptAndSpeakStreamingInProc(Prompt)`**
  - Requests a turn from the Director
  - Runs local LLM in-process with conversation history
  - Streams tokens and speaks sentence chunks as they're extracted
  - Uses the prompt wrapper to ensure dialogue output (not chat transcripts)

- **`SpeakTextLocal(Text)`**
  - Requests a turn from the Director
  - Skips the LLM and speaks the provided text directly via Piper
  - Useful for scripted lines or user speech

- **`Interrupt()`**
  - Cancels the active LLM generation and clears audio queues
  - Releases the turn immediately
  - Stops any playing audio

- **`ClearConversation()`**
  - Reserved for future use (conversation history is managed by the Subsystem)

#### Events

- **`OnToken(Token)`** - Fires as the LLM produces individual text tokens
- **`OnSpokenText(Text)`** - Fires when generation completes (full text)
- **`OnSubtitle(Speaker, Text)`** - Fires when a sentence is ready to be spoken (for subtitle UI)
- **`OnError(Error)`** - Fires on LLM or TTS errors

#### Properties

**Conversation:**
- `SpeakerName` - Name shown in subtitles and conversation history
- `ConversationRadius` - Distance for proximity grouping (default: 1500)
- `bTraceConversation` - Enable detailed logging for this character

**Streaming TTS:**
- `MinCharsBeforeSpeak` - Minimum characters before extracting a sentence (default: 24)
- `TurnReleaseAudioTailSeconds` - Time before force-stopping audio after turn ends (default: 0.2)

**Prompt/Personality:**
- `Directions` - Instructions for character behavior
- `Desc` - Character description

**Configuration:**
- `bUseProjectSettingsPaths` - Use project settings for file paths (default: true)
- `PathsOverride` - Override runtime paths
- `bUseProjectSettingsConfig` - Use project settings for LLM config (default: true)
- `CharacterConfigOverride` - Override LLM parameters (temperature, max tokens, GPU layers, etc.)

**Display:**
- `bShowOnScreenSubtitles` - Show subtitles on screen (default: true)

### `ULocalTalkConversationSubsystem` (The Director)

Access via Blueprint: `Get World Subsystem → Local Talk Conversation Subsystem`

#### Methods

- **`RequestTurn(Talker, Prompt)`** - Character requests to speak (called internally by component)
- **`ReleaseTurn(Talker)`** - Character finished speaking (called internally)
- **`BroadcastSentence(Speaker, Text, bFromUser)`** - Broadcast a sentence to nearby listeners (called internally)
- **`GetContextHistory(Agent)`** - Get conversation history for an agent's context
- **`InterruptProximity(Location, Radius)`** - Force-stop all characters near a location
- **`GetRegisteredTalkers()`** - Get all registered characters

---

## How It Works: Technical Details

### Prompt Wrapper System

The LLM uses a specialized prompt wrapper (`LocalTalkerInProcAsync::BuildPrompt`) that:

1. **Matches the model's chat template** (TinyLlama uses Zephyr-style: `<|system|>`, `<|user|>`, `<|assistant|>`)
2. **Includes clear output rules** in the system prompt:
   - Output only spoken dialogue (no speaker labels)
   - No control tokens or markup
   - Natural spacing and punctuation
   - Single paragraph preferred
3. **Sanitizes output** (`LocalTalkerSanitizeDialogueLine`):
   - Removes speaker prefixes ("Assistant:", "User:", "Name:", etc.)
   - Strips control tokens (`<|assistant|>`, `</s>`, `[INST]`, etc.)
   - Collapses whitespace
   - Converts newlines to spaces

This ensures characters speak naturally rather than outputting chat transcripts.

### Streaming TTS Pipeline

1. **LLM generates tokens** → accumulated in `LLMTextBuffer`
2. **Sentence extraction** (`ExtractAndEnqueueSentences`):
   - Waits for `MinCharsBeforeSpeak` characters
   - Extracts sentences on punctuation (`.`, `!`, `?`, `\n`)
   - Flushes remaining text when LLM completes
3. **Sentence queue** → Background TTS worker thread processes sentences
4. **Piper TTS** → Generates WAV file per sentence → loads PCM16 samples
5. **Audio chunks** → Queued to `AudioQueue` → `PumpAudioToProcedural` → `USoundWaveProcedural`
6. **Playback** → Unreal's audio system plays the procedural wave

The system uses thread-safe queues (`TQueue`) and counters to coordinate between game thread (LLM tokens), TTS worker thread (Piper), and audio thread (procedural wave).

### Conversation Subsystem (Director)

The `LocalTalkConversationSubsystem` manages conversations using a **Director pattern**:

1. **Registration**: Characters register on `BeginPlay`, unregister on `EndPlay`
2. **Proximity Groups**: Characters within each other's `ConversationRadius` form a `FLocalConversationContext`
   - Contexts have a center (average of participant locations)
   - Contexts update every tick to add/remove participants
   - Contexts are cleaned up after 30 seconds of inactivity
3. **Turn Management**:
   - Characters request turns via `RequestTurn` (queued in `ManualQueue`)
   - Director grants turns when no one in the context is busy
   - Characters release turns when finished (LLM done + TTS done + audio drained)
   - Only one speaker per context at a time
4. **Automatic Responses**:
   - When a turn is released, the Director evaluates if someone else should respond
   - Can trigger NPC responses to user messages or other NPCs (if enabled)
   - Respects `MaxConsecutiveNpcTurns` to prevent infinite loops
   - Can require a player listener (`bRequirePlayerListenerForAuto`)
   - Respects `MinSecondsBetweenAutoReplies` for pacing
5. **History Management**:
   - Each context maintains a rolling history (last 10 messages)
   - History is included in prompts for context
   - History is sanitized and formatted for better model conditioning

### LLM In-Process Generation

The `ULocalTalkerInProcGenerateAsync` node handles LLM generation:

1. **Model Cache**: Uses `FLocalTalkerLlamaCache` to share model instances across characters (single model loaded for all)
2. **GPU Offload**: Configurable via `GpuLayers` (0 = auto, >0 = offload N layers, <0 = CPU only)
3. **Streaming**: Tokens are dispatched as they're generated via `OnToken` event
4. **Early Stopping**: Stops at newlines, after reasonable length, or at terminal punctuation to keep output as dialogue
5. **Cancellation**: Supports cancellation via `Cancel()` method (used by `Interrupt()`)

---

## Configuration

### Project Settings (`ULocalTalkerSettings`)

Access via **Edit → Project Settings → LocalTalker**

**Paths (`DefaultPaths`):**
- `LlamaLibPath` - Path to `libllama.dll`
- `LlamaModelPath` - Path to `.gguf` model file
- `PiperExePath` - Path to `piper.exe`
- `PiperVoiceModelPath` - Path to voice `.onnx` file
- `WorkingDir` - Working directory for Piper (defaults to piper.exe directory)

**Character Config (`DefaultCharacterConfig`):**
- `Directions` - Character behavior instructions
- `CharacterDescription` - Character description
- `SystemPrompt` - System prompt override
- `Persona` - Character persona
- `MaxContextChars` - Maximum characters in context (default: 1600)
- `MaxHistoryMessages` - Maximum history messages (default: 16)
- `GpuLayers` - GPU layer offload (0 = auto, >0 = N layers, <0 = CPU only)
- `GpuBackend` - GPU backend preference (Auto, CPU, Vulkan, CUDA)
- `MaxTokens` - Maximum tokens to generate (default: 96 for dialogue)
- `Temperature` - Sampling temperature (default: 0.7)
- `Seed` - Random seed (0 = random)
- `Stop` - Stop sequence (empty by default)
- `bSpeak` - Enable TTS (default: true)
- `bStreamTokens` - Stream tokens as they're generated (default: true)

**Conversation (`Conversation`):**
- `bAllowNpcToNpcAuto` - Allow NPCs to automatically respond to each other (default: true)
- `MaxConsecutiveNpcTurns` - Cap on NPC-to-NPC turns (0 = unlimited, default: 0)
- `bRequirePlayerListenerForAuto` - Only auto-respond if player is nearby (default: true)
- `MinSecondsBetweenAutoReplies` - Minimum delay between auto-responses (default: 0.0)

**Voices (`Voices`):**
- Array of `FLocalTalkVoiceOption` for voice selection dropdowns

### Component Overrides

Each `ULocalCharacterComponent` can override project settings:

- `bUseProjectSettingsPaths` - Use project settings for paths (default: true)
- `PathsOverride` - Override individual paths (empty = use project settings/defaults)
- `bUseProjectSettingsConfig` - Use project settings for config (default: true)
- `CharacterConfigOverride` - Override individual config values

Path resolution order:
1. Component `PathsOverride` (if non-empty)
2. Project Settings `DefaultPaths`
3. Plugin default locations (if files exist)

---

## Preparing the Bundle (Building / Copying Third-Party)

### A) Recommended: use the helper script

Run from project root:

```powershell
.\Tools\PrepareLocalTalkerBundle.ps1
```

Notes:

- The script can **build llama.cpp** into `libllama.dll` if **CMake is installed** and `cmake` is on your PATH.
- Install CMake (or ensure Visual Studio CMake is available on PATH) to enable automated builds.

### B) Manual: build llama.cpp (Win64 DLL)

1. Get llama.cpp:
   - `git clone https://github.com/ggml-org/llama.cpp`
2. Build shared library with Vulkan support:

```powershell
mkdir build-win64
cd build-win64
cmake .. -DBUILD_SHARED_LIBS=ON -DGGML_VULKAN=ON
cmake --build . --config Release
```

3. Copy the resulting DLLs into:
   - `Plugins/LocalTalker/ThirdParty/llama/Win64/Release/`

### C) Manual: Piper

Place `piper.exe` (and any DLLs it depends on) into:

- `Plugins/LocalTalker/ThirdParty/piper/Win64/Release/`

Then place a voice model into:

- `Plugins/LocalTalker/Resources/Voices/`

### D) Licenses / notices

Third-party notices live here:

- `Plugins/LocalTalker/Resources/ThirdPartyNotices/`

Currently included:

- `llama.cpp_LICENSE.txt`
- `piper_LICENSE.txt`
- `tinyllama_LICENSE_NOTICE.txt`

---

## Troubleshooting

### "Failed to load libllama.dll"

- Confirm `libllama.dll` exists at the expected path or set `DefaultPaths.LlamaLibPath`.
- If llama.cpp has additional dependent DLLs, ensure they are next to `libllama.dll` or in PATH.

### "Failed to load model"

- Confirm the `.gguf` exists and the path is correct.
- **Check file size** — the model should be hundreds of MB. If it's only a few MB, it's likely a corrupt/incomplete download.
- Re-download from: [TinyLlama Q4_K_M](https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf)
- **"no backends are loaded" error**: This indicates that llama.cpp backends (CPU/GPU) aren't being registered. Ensure:
  - `ggml-cpu.dll` is present and loads successfully
  - The llama.cpp DLLs were built with backend auto-registration enabled
  - If using a custom build, ensure backends are configured to auto-register on DLL load

### "piper failed …"

- Confirm `piper.exe` exists.
- Confirm the voice `.onnx` (and often `.onnx.json`) exists and is compatible with the piper build.
- Check stderr in the logged error message.

### GPU not being used / slow inference

- Confirm `ggml-vulkan.dll` is present in `ThirdParty/llama/Win64/Release/`
- Check Unreal log for "GPU offload is supported"
- Ensure Vulkan drivers are installed (run `vulkaninfo` from command line to verify)
- Check `GpuLayers` setting (0 = auto, >0 = offload N layers)

### Character not speaking / errors

- Check Unreal Output Log for error messages
- Verify all paths are set correctly (Project Settings or component overrides)
- Ensure model and voice files are valid (run automation tests)
- Check `OnError` event for component-specific errors

### Conversation not working / characters not responding

- Ensure characters have `LocalCharacterComponent` added
- Check `ConversationRadius` is set appropriately (default: 1500)
- Verify characters are within each other's `ConversationRadius`
- Check `bAllowNpcToNpcAuto` in Project Settings if expecting NPC-to-NPC responses
- Enable `bTraceConversation` on components or set console variable `LocalTalker.TraceConversation=1` for detailed logs

### Debugging Tips

- **Console Variables:**
  - `LocalTalker.TraceConversation=1` - Enable detailed conversation logging
- **Component Properties:**
  - `bTraceConversation` - Enable tracing for specific character
  - `bShowOnScreenSubtitles` - Show subtitles for debugging
- **Logs:**
  - Check `LogLocalTalker` category in Output Log
  - Look for `[Director]` and `[TalkTrace]` prefixed messages

---

## What's Next (Known Follow-Ups)

- Persist a single Piper worker process to avoid per-sentence process startup overhead
- Add microphone input + optional STT (Speech-to-Text) backend
- CUDA support for NVIDIA GPUs (currently Vulkan-only for cross-vendor compatibility)
- Voice selection UI (component dropdown to select from configured voices)
- More sophisticated turn-taking logic (priority, personality-based selection)
