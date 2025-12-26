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
    void RequestTurn(ULocalCharacterComponent* Talker, const FString& Prompt);
    
    /** Character finished speaking. */
    void ReleaseTurn(ULocalCharacterComponent* Talker);

    /** Broadcast a sentence from a speaker to nearby agents. */
    void BroadcastSentence(ULocalCharacterComponent* Speaker, const FString& Text, bool bFromUser);

    /** Gets the history for the context this agent belongs to. */
    TArray<FLocalTalkMessage> GetContextHistory(ULocalCharacterComponent* Agent);

    /** Forces everyone near a location to stop talking. */
    void InterruptProximity(const FVector& Location, float Radius);

    // --- Queries ---
    TArray<ULocalCharacterComponent*> GetRegisteredTalkers() const;

private:
    UPROPERTY()
    TSet<TWeakObjectPtr<ULocalCharacterComponent>> Registry;

    UPROPERTY()
    TArray<FLocalConversationContext> ActiveContexts;

    struct FQueuedTurn
    {
        TWeakObjectPtr<ULocalCharacterComponent> Talker;
        FString Prompt;
    };
    TArray<FQueuedTurn> ManualQueue;

    void UpdateContexts();
    void ProcessTurns();
    
    FLocalConversationContext* FindOrCreateContext(ULocalCharacterComponent* Agent);
    void AddMessageToContext(FLocalConversationContext& Context, const FString& Speaker, const FString& Text, bool bFromUser);
    
    // Logic to decide who should respond next in a context
    void EvaluateNextSpeaker(FLocalConversationContext& Context, ULocalCharacterComponent* LastSpeaker);
};
