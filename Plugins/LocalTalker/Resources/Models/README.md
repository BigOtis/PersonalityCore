# Bundled LLM model notes

This folder contains GGUF language models for LocalTalker's llama.cpp dialogue generation.

Current default behavior:

- `BundledModelFile` in `Project Settings -> LocalTalker` selects the model by filename.
- If `BundledModelFile` is empty or missing, runtime falls back to:
  - `Resources/Models/Llama-3.2-3B-Instruct-Q6_K_L.gguf`

When adding or replacing models:

1. Put `.gguf` files in this folder.
2. Set `BundledModelFile` to the target filename.
3. Run automation tests to confirm runtime load paths.
