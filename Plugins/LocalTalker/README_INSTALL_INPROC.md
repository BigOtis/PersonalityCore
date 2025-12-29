# LocalTalker (llama.cpp in-process) - Win64 quickstart

This package switches the default LLM backend to llama.cpp in-process.

## What you still need to provide
For licensing and size reasons, this zip does NOT ship a model or llama.cpp binaries.

You must provide:
- A GGUF model file (default recommended below)
- A built libllama (dll + import lib) or static lib, plus headers, from llama.cpp

## Default recommended model
Model family: Llama 3.2 3B (GGUF)
Quantization: Q4_K_M (balanced, ~2.02GB)

Filename:
  Llama-3.2-3B-Q4_K_M.gguf

Download:
  https://huggingface.co/tensorblock/Llama-3.2-3B-GGUF/resolve/main/Llama-3.2-3B-Q4_K_M.gguf

Example location:
  C:\AI\models\Llama-3.2-3B-Q4_K_M.gguf

## Building libllama on Windows
1) Clone llama.cpp:
   git clone https://github.com/ggml-org/llama.cpp

2) Build DLL (CMake preset varies by version). A common pattern:
   mkdir build && cd build
   cmake .. -DBUILD_SHARED_LIBS=ON
   cmake --build . --config Release

3) Copy outputs into the plugin:
   Plugins/LocalTalker/ThirdParty/llama/Win64/Release/
     - libllama.dll
     - libllama.lib (import lib, optional for runtime-only)
   Plugins/LocalTalker/ThirdParty/llama/include/
     - llama.h (+ any headers it includes)

## Configure Unreal Project Settings
Edit -> Project Settings -> LocalTalker

- DefaultPaths.LlamaModelPath = your GGUF path
- DefaultPaths.LlamaLibPath   = absolute path to libllama.dll
  Example: C:\YourProject\Plugins\LocalTalker\ThirdParty\llama\Win64\Release\libllama.dll

- Piper paths (same as before) for TTS.

## Test
Add LocalCharacterComponent to an Actor.
Call SendPromptAndSpeakStreamingInProc("Hello") on BeginPlay.

If it errors, check:
- libllama.dll exists and is loadable
- dependent DLLs (if any) are in the same folder
- model path is correct
