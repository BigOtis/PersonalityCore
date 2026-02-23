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
    QwenWorker UMETA(DisplayName="Qwen3-TTS Worker"),
    PiperLegacy UMETA(DisplayName="Piper (Legacy, Deprecated)")
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

    // --- Qwen3-TTS runtime ---
    // Python executable used to launch the bundled worker script.
    // Examples: "python", "python3", "C:/Python312/python.exe"
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Qwen")
    FString QwenPythonExePath = TEXT("python");

    // Absolute or plugin-relative path to qwen_tts_worker.py.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Qwen")
    FString QwenWorkerScriptPath;

    // HuggingFace repo id or local model folder.
    // Example: "Qwen/Qwen3-TTS-12Hz-0.6B-CustomVoice"
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Qwen")
    FString QwenModelPath = TEXT("Qwen/Qwen3-TTS-12Hz-0.6B-CustomVoice");

    // Optional HuggingFace repo id or local tokenizer folder.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Qwen")
    FString QwenTokenizerPath = TEXT("Qwen/Qwen3-TTS-Tokenizer-12Hz");

    // Example: "cuda:0", "cuda", "cpu"
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Qwen")
    FString QwenDevice = TEXT("cuda:0");

    // One of: "bfloat16", "float16", "float32"
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Qwen")
    FString QwenDType = TEXT("bfloat16");

    // Default language passed to Qwen generation APIs, usually "Auto".
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Qwen")
    FString QwenLanguage = TEXT("English");

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

    // Qwen CustomVoice speaker name (as recognized by the loaded model).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Voices|Qwen")
    FString QwenSpeaker;

    // Optional generation instruction to shape delivery/emotion.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Voices|Qwen")
    FString QwenInstruction;

    // Optional externally-generated voice prompt file path for Base model voice clone inference.
    // The plugin does not create these prompts; users generate them offline and include them.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Voices|Qwen")
    FString QwenVoicePromptPath;

    // Legacy Piper fields kept for backward compatibility.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Voices")
    FString VoiceOnnxPath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Voices")
    FString VoiceJsonPath;
};
