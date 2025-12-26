# LocalTalker (UE 5.5) — Local LLM + Local TTS (Streaming)

LocalTalker is a **Blueprint-first** UE plugin that lets you drive interactive characters using:

- **Local LLM (llama.cpp)** — in-process, streaming tokens with GPU acceleration
- **Local TTS (Piper)** — generates WAV audio and plays it via `USoundWaveProcedural`

This repo currently focuses on **Win64 / UE 5.5**.

> Current state: LLM streaming is implemented in-process with Vulkan GPU acceleration support. TTS works via Piper CLI per sentence (fast enough to test; we'll later optimize to avoid per-sentence process startup overhead).

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

## Quick Start (Testing Current State)

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

### 3) (Optional) Configure Project Settings overrides

Open:

- **Edit → Project Settings → LocalTalker**

Set:

- `DefaultPaths.LlamaLibPath`
- `DefaultPaths.LlamaModelPath`
- `DefaultPaths.PiperExePath`
- `DefaultPaths.PiperVoiceModelPath`
- `DefaultPaths.WorkingDir` (optional; defaults to plugin folder)

Per-character overrides also exist on the component.

### 4) Add a character component and test streaming speech

1. Create an `Actor` (or use an existing actor)
2. Add **LocalCharacterComponent**
3. On BeginPlay, call:
   - `SendPromptAndSpeakStreamingInProc("Hello!")`

What you should see/hear:

- **Streaming text**: `OnToken` fires as tokens arrive.
- **Streaming speech**: TTS begins once sentence chunks are extracted (punctuation or flush timeout).

---

## Blueprint API (Core)

### `ULocalCharacterComponent`

- **`SendPromptAndSpeakStreamingInProc(Prompt)`**
  - Runs local LLM in-process
  - Streams tokens and speaks sentence chunks
- **`SpeakTextLocal(Text)`**
  - Skips the LLM and only speaks the provided text via Piper
- **`Interrupt()`**
  - Cancels the active generation and clears audio queues

Events:

- **`OnToken(Token)`** — fires as the LLM produces text pieces
- **`OnSpokenText(Text)`** — fires when the generation completes (full text)
- **`OnError(Error)`**

Streaming TTS tuning (per component):

- `bSpeakStreaming` (default true)
- `MinCharsBeforeSpeak`
- `FlushSeconds`
- `MaxSentenceChars`

---

## Preparing the bundle (building / copying third-party)

### A) Recommended: use the helper script

Run from project root:

```powershell
.\Tools\PrepareLocalTalkerBundle.ps1
```

Notes:

- The script can **build llama.cpp** into `libllama.dll` if **CMake is installed** and `cmake` is on your PATH.
- On this machine we detected **Git is installed** but **CMake was not on PATH**. Install CMake (or ensure Visual Studio CMake is available on PATH) to enable automated builds.

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

When bundling Piper, add:

- `piper_LICENSE.txt` (replace the placeholder file)

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

---

## What's next (known follow-ups)

- Persist a single Piper worker process to avoid per-sentence process startup overhead.
- Add a higher-level conversation/orchestration component for multi-character scenes.
- Add microphone input + optional STT backend.
- CUDA support for NVIDIA GPUs (currently Vulkan-only for cross-vendor compatibility).
