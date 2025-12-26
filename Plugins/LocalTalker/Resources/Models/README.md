# Bundled LLM models

This folder is where the Fab distribution of **LocalTalker** will ship one or more default GGUF models for llama.cpp.

The runtime will auto-default to:

- `Resources/Models/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf`

If you change the bundled model name(s), update the defaults in:

- `Source/LocalTalker/Private/LocalCharacterComponent.cpp` (`ResolvePaths()`)


