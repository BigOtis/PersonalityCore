#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "LocalTalkerTypes.h"
#include "LocalTalkerSettings.generated.h"

UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="LocalTalker"))
class LOCALTALKER_API ULocalTalkerSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    ULocalTalkerSettings();
    virtual FName GetCategoryName() const override;
    virtual FName GetSectionName() const override;

    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Paths")
    FLocalTalkerRuntimePaths DefaultPaths;

    // Optional: pick a GGUF model bundled in Resources/Models via dropdown.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Paths", meta=(GetOptions="GetAvailableModelOptions"))
    FString BundledModelFile;

    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Defaults")
    FLocalTalkerCharacterConfig DefaultCharacterConfig;

    // Active TTS backend.
    // KokoroWorker = CPU-only ONNX, no GPU required, auto-downloads ~380 MB model on first run.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="TTS")
    ELocalTalkTtsBackend TtsBackend = ELocalTalkTtsBackend::KokoroWorker;

    // --- Kokoro ONNX settings ---

    // Number of parallel Kokoro worker processes (1 is sufficient for real-time CPU synthesis).
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="TTS|Kokoro", meta=(ClampMin="1", ClampMax="4"))
    int32 KokoroWorkerPoolSize = 1;

    // Timeout per Kokoro TTS request (seconds). Kokoro is fast on CPU; 30s is generous.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="TTS|Kokoro", meta=(ClampMin="5.0"))
    float KokoroRequestTimeoutSeconds = 60.0f;

    // Global speech speed multiplier. 1.0 = normal, 1.2 = 20% faster.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="TTS|Kokoro", meta=(ClampMin="0.5", ClampMax="2.0"))
    float KokoroSpeed = 1.0f;

    // Fallback Kokoro voice used when a character has no KokoroVoice configured.
    // Available voices: af_bella, af_nova, af_sarah, am_adam, am_michael, am_eric, bm_george, etc.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="TTS|Kokoro")
    FString KokoroDefaultVoice = TEXT("af_bella");

    // --- Whisper STT settings ---
    // Whisper model size/name for faster-whisper (e.g. tiny.en, base.en, small.en, medium.en, large-v3-turbo).
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="STT|Whisper")
    FString WhisperModel = TEXT("base.en");

    // Language hint (ISO code). Empty = auto-detect.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="STT|Whisper")
    FString WhisperLanguage = TEXT("en");

    // If true, faster-whisper VAD filtering is enabled.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="STT|Whisper")
    bool bWhisperVadFilter = true;

    // Timeout per STT request (seconds), including model warmup on first request.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="STT|Whisper", meta=(ClampMin="5.0"))
    float WhisperRequestTimeoutSeconds = 90.0f;

    // Bundled/local voice options (used to populate component dropdown).
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Voices")
    TArray<FLocalTalkVoiceOption> Voices;

    // --- Microphone defaults (for upcoming player mic/STT support) ---
    // End-user setting friendly: choose system default mic or a named device.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Microphone")
    ELocalTalkMicInputDeviceMode MicInputDeviceMode = ELocalTalkMicInputDeviceMode::DefaultSystem;

    // Device name to use when MicInputDeviceMode is Specific Device.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Microphone", meta=(GetOptions="GetMicInputDeviceOptions"))
    FString MicInputDeviceName;

    // --- Conversation defaults (Director behavior) ---
    // When enabled, nearby NPCs can auto-respond to each other (not just to USER messages).
    // This is gated by MaxConsecutiveNpcTurns to avoid runaway loops.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Conversation")
    bool bAllowNpcToNpcAuto = true;

    // Safety cap: after this many consecutive non-user messages in a context, the Director will stop auto-triggering.
    // Set to 0 for unlimited (recommended when bRequirePlayerListenerForAuto is enabled).
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Conversation", meta=(ClampMin="0"))
    int32 MaxConsecutiveNpcTurns = 6;

    // If enabled, NPC-to-NPC auto conversation will only run while at least one local player pawn
    // is within hearing range of the conversation participants.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Conversation")
    bool bRequirePlayerListenerForAuto = true;

    // If enabled, ALL speech (including direct/manual prompts) is gated by a local player listener
    // being within hearing range of the conversation participants.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Conversation")
    bool bRequirePlayerListenerForAllTalk = true;

    // Optional pacing: the Director won't auto-trigger until at least this much time has passed since the last message.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Conversation", meta=(ClampMin="0.0"))
    float MinSecondsBetweenAutoReplies = 0.10f;

    // Random pause (0 to this many seconds) after an AI finishes talking before the next speaker is granted a turn. Gives the player a chance to speak.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Conversation", meta=(ClampMin="0.0", ClampMax="30.0"))
    float PostTurnPauseMaxSeconds = 5.0f;

    // Keep conversations alive indefinitely: if a context goes quiet, the Director will auto-trigger
    // a new turn after MaxSilenceSeconds (even if the last message is old).
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Conversation")
    bool bKeepConversationAlive = true;

    // Maximum time (seconds) a conversation context is allowed to be silent before we auto-trigger a new line.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Conversation", meta=(ClampMin="0.0"))
    float MaxSilenceSeconds = 5.0f;

    // When keep-alive is enabled, optionally ignore the player-listener gate (useful for kiosk / background chatter scenes).
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Conversation")
    bool bKeepAliveIgnoresPlayerListenerRequirement = true;

    // How long a context must be inactive before we delete it. If keep-alive is enabled, contexts are not cleaned up.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Conversation", meta=(ClampMin="0.0"))
    float ContextCleanupSeconds = 30.0f;

    // When DefaultCharacterConfig.GpuLayers is 0 (auto), cap the number of GPU layers to avoid VRAM exhaustion.
    // Set to 0 to allow full offload.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Performance", meta=(ClampMin="0"))
    int32 AutoGpuLayerCap = 24;

    // Dynamic context sizing (tokens). The runtime will clamp n_ctx to this range.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Performance", meta=(ClampMin="128"))
    int32 MinContextTokens = 512;

    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Performance", meta=(ClampMin="128"))
    int32 MaxContextTokens = 2048;

    // Extra headroom for generation (tokens) when sizing n_ctx.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Performance", meta=(ClampMin="0"))
    int32 ContextTokenMargin = 64;

    UFUNCTION()
    TArray<FString> GetAvailableModelOptions() const;

    UFUNCTION()
    TArray<FString> GetMicInputDeviceOptions() const;
};
