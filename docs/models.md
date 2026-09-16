# Local inference providers

LocalTalker connects to providers rather than requiring one model or inference engine.

| Provider | Setup |
| --- | --- |
| Ollama | Run Ollama, install a model, and select it in the studio. Default API: http://127.0.0.1:11434/v1. |
| llama.cpp | Configure llama_server_path and a local GGUF llama_model_path; the runtime can start the server. |
| OpenAI-compatible | Configure the URL and model identifier supplied by your server. Remote endpoints send requests off-device. |
| Simulated | Explicit test provider for exercising the interface without model weights. |

Use **Inspect → setup** in the Windows studio. Settings are stored in %LOCALAPPDATA%\LocalTalker\config.json unless LOCALTALKER_HOME is set.

The catalog also offers a Muse Glimmer GGUF installation option. It downloads substantial model assets separately; they are not included in the framework or desktop release. Memory, speed, context limits, and model licenses depend on the selected model and quantization. Adjust GPU offload and context settings for the machine sharing resources with your game.

An unavailable real provider reports an error. LocalTalker does not silently substitute the simulated provider. Speech recognition and voice synthesis use separate assets; see [speech](speech.md).
