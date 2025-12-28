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
    int32 MaxTokens = 96;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    float Temperature = 0.7f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    int32 Seed = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    FString Stop; // Empty by default - let model generate naturally, we'll clean output post-processing

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

    // Stable ID shown in the component dropdown (ex: "en_US-lessac-small")
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Voices")
    FName Id = NAME_None;

    // Absolute or plugin-relative path to the .onnx voice file.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Voices")
    FString VoiceOnnxPath;

    // Optional: voice metadata json (not required by piper.exe, but useful for UI).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Voices")
    FString VoiceJsonPath;
};
