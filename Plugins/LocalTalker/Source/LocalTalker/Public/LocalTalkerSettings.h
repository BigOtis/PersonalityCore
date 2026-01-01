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

    // Bundled/local voice options (used to populate component dropdown).
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Voices")
    TArray<FLocalTalkVoiceOption> Voices;

    // --- Conversation defaults (Director behavior) ---
    // When enabled, nearby NPCs can auto-respond to each other (not just to USER messages).
    // This is gated by MaxConsecutiveNpcTurns to avoid runaway loops.
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Conversation")
    bool bAllowNpcToNpcAuto = true;

    // Safety cap: after this many consecutive non-user messages in a context, the Director will stop auto-triggering.
    // Set to 0 for unlimited (recommended when bRequirePlayerListenerForAuto is enabled).
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Conversation", meta=(ClampMin="0"))
    int32 MaxConsecutiveNpcTurns = 0;

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
};
