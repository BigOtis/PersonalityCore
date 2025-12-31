# Bundled LLM models

This folder is where the Fab distribution of **LocalTalker** will ship one or more default GGUF models for llama.cpp.

The runtime will auto-default to:

- `Resources/Models/Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf`

If you change the bundled model name(s), update the defaults in:

- `Source/LocalTalker/Private/LocalCharacterComponent.cpp` (`ResolvePaths()`)

