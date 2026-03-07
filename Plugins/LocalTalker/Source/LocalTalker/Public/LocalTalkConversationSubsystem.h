#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "LocalTalkerTypes.h"
#include "LocalTalkConversationSubsystem.generated.h"

class ULocalCharacterComponent;

USTRUCT(BlueprintType)
struct FLocalTalkMessage
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, Category="LocalTalker")
    FString SpeakerName;

    UPROPERTY(BlueprintReadWrite, Category="LocalTalker")
    FString Content;

    UPROPERTY(BlueprintReadWrite, Category="LocalTalker")
    bool bFromUser = false;
};

/**
 * Represents a group of agents talking near each other.
 */
USTRUCT(BlueprintType)
struct FLocalConversationContext
{
    GENERATED_BODY()

    UPROPERTY()
    TArray<TWeakObjectPtr<ULocalCharacterComponent>> Participants;

    UPROPERTY()
    TArray<FLocalTalkMessage> History;

    UPROPERTY()
    FVector LastCenter = FVector::ZeroVector;

    UPROPERTY()
    float LastInteractionTime = 0.0f;

    // Counts consecutive non-user messages since the last user message.
    // Used to cap NPC-to-NPC auto-response loops.
    UPROPERTY()
    int32 ConsecutiveNpcTurns = 0;

    // Used for keep-alive auto-chatter to avoid re-enqueueing every tick.
    UPROPERTY()
    float LastAutoEnqueueTime = 0.0f;

    // Throttle loop-guard logs per context.
    UPROPERTY()
    float LastLoopGuardLogTime = 0.0f;
};

/**
 * The "Director" of the conversation. 
 * Manages proximity groups and dictates who speaks and when.
 */
UCLASS()
class LOCALTALKER_API ULocalTalkConversationSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;

    // --- Registry ---
    void RegisterTalker(ULocalCharacterComponent* Talker);
    void UnregisterTalker(ULocalCharacterComponent* Talker);

    // --- Turn Management ---
    /** Character wants to say something (e.g. triggered by player interaction). */
    void RequestTurn(ULocalCharacterComponent* Talker, const FString& Prompt, bool bFromUser = false);
    
    /** Character finished speaking. */
    void ReleaseTurn(ULocalCharacterComponent* Talker);

    /** Broadcast a sentence from a speaker to nearby agents. */
    void BroadcastSentence(ULocalCharacterComponent* Speaker, const FString& Text, bool bFromUser);

    /** Gets the history for the context this agent belongs to. */
    TArray<FLocalTalkMessage> GetContextHistory(ULocalCharacterComponent* Agent);

    /** Gets the current participants for the context this agent belongs to. */
    TArray<ULocalCharacterComponent*> GetContextParticipants(const ULocalCharacterComponent* Agent) const;

    /** Clears the history for the context this agent belongs to. */
    void ClearContextHistory(ULocalCharacterComponent* Agent);

    // Internal: notify participants when a talker finishes audio so pending playback can start.
    void NotifyAudioFinished(ULocalCharacterComponent* Talker);

    /** Forces everyone near a location to stop talking. */
    void InterruptProximity(const FVector& Location, float Radius);

    /** Removes queued turns for talkers near a location (used to prioritize fresh player input). */
    void CancelQueuedTurnsInProximity(const FVector& Location, float Radius);

    /**
     * Marks a temporary "player priority" window around a location.
     * While active, NPC auto-turns in this area are deferred so player speech can be handled first.
     */
    void SetPlayerSpeechPriorityWindow(const FVector& Location, float Radius, float HoldSeconds);

    /**
     * Hard suppression fence while player speech is being captured/transcribed.
     * NPC turns in-range are paused until EndPlayerSpeechFence is called (or timeout).
     */
    void BeginPlayerSpeechFence(const FVector& Location, float Radius, float MaxHoldSeconds);
    void EndPlayerSpeechFence();

    // --- Queries ---
    TArray<ULocalCharacterComponent*> GetRegisteredTalkers() const;
    bool HasPlayerListenerInRange(const ULocalCharacterComponent* Talker) const;

private:
    UPROPERTY()
    TSet<TWeakObjectPtr<ULocalCharacterComponent>> Registry;

    UPROPERTY()
    TArray<FLocalConversationContext> ActiveContexts;

    struct FQueuedTurn
    {
        TWeakObjectPtr<ULocalCharacterComponent> Talker;
        FString Prompt;
        bool bFromUser = false;
        double EarliestGrantWorldSeconds = 0.0;
    };
    TArray<FQueuedTurn> ManualQueue;

    void UpdateContexts();
    void ProcessTurns();
    void MaintainKeepAlive();
    void RefreshContextParticipants(FLocalConversationContext& Context);

    void EnqueueTurn(ULocalCharacterComponent* Talker, const FString& Prompt, double EarliestGrantWorldSeconds, bool bFromUser);
    
    FLocalConversationContext* FindOrCreateContext(ULocalCharacterComponent* Agent);
    void AddMessageToContext(FLocalConversationContext& Context, const FString& Speaker, const FString& Text, bool bFromUser);

    bool IsPlayerSpeechPriorityActive(double NowWorldSeconds) const;
    bool IsPlayerSpeechFenceActive(double NowWorldSeconds) const;
    bool IsTalkerWithinPlayerSpeechFence(const ULocalCharacterComponent* Talker, double NowWorldSeconds) const;
    bool IsContextWithinPlayerSpeechFence(const FLocalConversationContext& Context, double NowWorldSeconds) const;
    bool IsTalkerWithinPlayerPriorityWindow(const ULocalCharacterComponent* Talker, double NowWorldSeconds) const;
    bool IsContextWithinPlayerPriorityWindow(const FLocalConversationContext& Context, double NowWorldSeconds) const;
    
    // Logic to decide who should respond next in a context
    void EvaluateNextSpeaker(FLocalConversationContext& Context, ULocalCharacterComponent* LastSpeaker);

    bool bPlayerSpeechPriorityActive = false;
    FVector PlayerSpeechPriorityCenter = FVector::ZeroVector;
    float PlayerSpeechPriorityRadius = 0.0f;
    double PlayerSpeechPriorityUntilWorldSeconds = 0.0;
    double NextPlayerPriorityBlockedLogWorldSeconds = 0.0;

    bool bPlayerSpeechFenceActive = false;
    FVector PlayerSpeechFenceCenter = FVector::ZeroVector;
    float PlayerSpeechFenceRadius = 0.0f;
    double PlayerSpeechFenceUntilWorldSeconds = 0.0;
};
