#pragma once

#include "CoreMinimal.h"
#include "LocalTalkerTypes.generated.h"

UENUM(BlueprintType)
enum class ELocalTalkerGpuBackend : uint8
{
    Auto UMETA(DisplayName="Auto (Prefer Vulkan)"),
    CPU  UMETA(DisplayName="CPU Only"),
    Vulkan UMETA(DisplayName="Vulkan"),
    CUDA UMETA(DisplayName="CUDA (NVIDIA Only)")
};

UENUM(BlueprintType)
enum class ELocalTalkMicInputDeviceMode : uint8
{
    DefaultSystem UMETA(DisplayName="Default (System)"),
    NamedDevice UMETA(DisplayName="Specific Device")
};

UENUM(BlueprintType)
enum class ELocalTalkTtsBackend : uint8
{
    KokoroWorker UMETA(DisplayName="Kokoro ONNX (CPU, Recommended)"),
    PiperLegacy  UMETA(DisplayName="Piper (Legacy, Deprecated)")
};

USTRUCT(BlueprintType)
struct FLocalTalkerCharacterConfig
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Prompt")
    FString Directions;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Prompt")
    FString CharacterDescription;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    FString SystemPrompt;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    FString Persona;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Prompt")
    int32 MaxContextChars = 1600;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Prompt")
    int32 MaxHistoryMessages = 16;

    // 0 = auto (uses backend rules below), <0 = force CPU, >0 = offload up to N layers.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Performance")
    int32 GpuLayers = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Performance")
    ELocalTalkerGpuBackend GpuBackend = ELocalTalkerGpuBackend::Auto;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    // Dialogue is intended to be short (one line). Keep this modest to reduce loops and long stalls.
    int32 MaxTokens = 64;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    float Temperature = 0.40f;

    // --- Sampling controls (best-practice defaults for chatty dialogue without echo loops) ---
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Sampling", meta=(ClampMin="0"))
    int32 TopK = 40;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Sampling", meta=(ClampMin="0.0", ClampMax="1.0"))
    float TopP = 0.88f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Sampling", meta=(ClampMin="0.0", ClampMax="1.0"))
    float MinP = 0.05f; // 0 disables min-p

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Sampling", meta=(ClampMin="0.0", ClampMax="1.0"))
    float TypicalP = 1.0f; // 1 disables typical-p

    // Repetition / presence / frequency penalties (llama.cpp penalties sampler).
    // NOTE: penalties are applied after top-k/top-p for performance.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Sampling", meta=(ClampMin="0"))
    int32 RepeatLastN = 128;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Sampling", meta=(ClampMin="1.0"))
    float RepeatPenalty = 1.15f; // 1.0 disables

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Sampling", meta=(ClampMin="0.0"))
    float FrequencyPenalty = 0.14f; // 0 disables

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Sampling", meta=(ClampMin="0.0"))
    float PresencePenalty = 0.06f; // 0 disables

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    int32 Seed = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    FString Stop; // Empty by default - let model generate naturally, we'll clean output post-processing

    // Optional multi-stop support. If provided, overrides Stop.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    TArray<FString> StopSequences;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    bool bSpeak = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    bool bStreamTokens = true;
};

USTRUCT(BlueprintType)
struct FLocalTalkerRuntimePaths
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    FString LlamaModelPath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    FString LlamaLibPath;

    // --- Kokoro ONNX runtime ---
    // Python executable used to launch the Kokoro worker.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Kokoro")
    FString KokoroPythonExePath = TEXT("python");

    // Absolute or plugin-relative path to kokoro_tts_worker.py.
    // Leave empty to auto-resolve from Resources/Kokoro/ inside the plugin.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Kokoro")
    FString KokoroWorkerScriptPath;

    // Directory to cache downloaded Kokoro model files.
    // Leave empty to use %LOCALAPPDATA%\LocalTalker\Kokoro (Windows) or ~/.cache/localtalker/kokoro.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Kokoro")
    FString KokoroCacheDir;

    // --- Whisper STT runtime ---
    // Python executable used to launch the Whisper worker.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Whisper")
    FString WhisperPythonExePath = TEXT("python");

    // Absolute or plugin-relative path to whisper_stt_worker.py.
    // Leave empty to auto-resolve from Resources/Whisper/ inside the plugin.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Whisper")
    FString WhisperWorkerScriptPath;

    // Directory used by Whisper/faster-whisper to cache downloaded model files.
    // Leave empty to use the worker default cache location.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Whisper")
    FString WhisperCacheDir;

    // --- Legacy Piper runtime (deprecated) ---
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    FString PiperExePath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    FString PiperVoiceModelPath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    FString WorkingDir;
};

USTRUCT(BlueprintType)
struct FLocalTalkVoiceOption
{
    GENERATED_BODY()

    // Stable ID shown in component dropdowns.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Voices")
    FName Id = NAME_None;

    // Kokoro ONNX voice name (e.g. "af_bella", "am_michael").
    // See https://huggingface.co/hexgrad/Kokoro-82M for full list.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Voices|Kokoro")
    FString KokoroVoice;

    // Legacy Piper fields kept for backward compatibility.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Voices")
    FString VoiceOnnxPath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Voices")
    FString VoiceJsonPath;
};
