#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"

#include "LocalTalkConversationSubsystem.generated.h"

class ULocalCharacterComponent;

/**
 * Manages turn-taking and conversation flow between multiple LocalCharacterComponents.
 *
 * Goals:
 * - Never cut off audio unless explicitly interrupted by the player.
 * - Prevent overlapping speech within proximity.
 * - Keep NPC-to-NPC chatter realistic (not constant ping-pong).
 * - Always respond to player/user utterances.
 * - Optional idle chatter after long silence.
 */
UCLASS()
class LOCALTALKER_API ULocalTalkConversationSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;

    void RegisterTalker(ULocalCharacterComponent* Talker);
    void UnregisterTalker(ULocalCharacterComponent* Talker);

    // Called when a talker begins generating/speaking (LLM starts)
    void NotifyStartedSpeaking(ULocalCharacterComponent* Speaker);

    // Called for each sentence as it's generated (for live broadcasting)
    void NotifySentenceSpoken(ULocalCharacterComponent* Speaker, const FString& Sentence, bool bFromUser);

    // Called when LLM generation is complete (but audio may still be playing!)
    void NotifyFinishedSpeaking(ULocalCharacterComponent* Speaker);

    // Called when audio playback has actually finished - THIS triggers next turn
    void NotifyAudioPlaybackFinished(ULocalCharacterComponent* Speaker);

    // Called when interrupted mid-speech
    void NotifyInterrupted(ULocalCharacterComponent* Speaker);

    // Broadcast user input to nearby talkers
    void BroadcastUserUtterance(const FVector& Location, float Radius, const FString& UserText, bool bInterrupt);

    bool IsAnyoneSpeakingNear(const FVector& Location, float Radius) const;
    bool IsSpeaking(ULocalCharacterComponent* Talker) const;

private:
    enum class ETalkerState : uint8
    {
        Idle,
        Generating,
        PlayingAudio,
    };

    struct FTalkerState
    {
        ETalkerState State = ETalkerState::Idle;
        double StateStartTime = 0.0;
        double LastSpokeTime = 0.0; // when they last finished a full turn
        int32 RecentTurnCount = 0;
    };

    struct FPendingUtterance
    {
        FString SpeakerName;
        FString Text;
        FVector Location = FVector::ZeroVector;
        float Radius = 0.0f;
        double TimeReceived = 0.0;
        double ResponseDueTime = 0.0;
        bool bFromUser = false;

        bool bResponderSelected = false;
        TWeakObjectPtr<ULocalCharacterComponent> SelectedResponder;
    };

    TSet<TWeakObjectPtr<ULocalCharacterComponent>> Talkers;
    TMap<TWeakObjectPtr<ULocalCharacterComponent>, FTalkerState> TalkerStates;
    TMap<TWeakObjectPtr<ULocalCharacterComponent>, FString> AggregatedBySpeaker;
    TArray<FPendingUtterance> PendingUtterances;

    // Global pacing / fairness knobs
    float TurnCooldownSeconds = 1.25f;
    float RecentTurnWindow = 8.0f;

    // Activity tracking / idle chatter scheduling
    double LastConversationActivitySeconds = 0.0;
    double NextIdleChatterDueSeconds = 0.0;

    void Compact();
    void UpdateTalkerStates();
    void ProcessPendingUtterances();
    void SelectResponderForUtterance(FPendingUtterance& Utterance);
    void DispatchResponse(ULocalCharacterComponent* Responder, const FPendingUtterance& Utterance);
    float CalculateResponsePriority(ULocalCharacterComponent* Candidate, const FPendingUtterance& Utterance) const;
    bool CanRespond(ULocalCharacterComponent* Talker) const;

    void TryStartIdleChatter();
    void RescheduleIdleChatter(double Now);

    void SetTalkerState(ULocalCharacterComponent* Talker, ETalkerState NewState);
    FTalkerState& GetOrCreateTalkerState(ULocalCharacterComponent* Talker);

    void CancelSpeakingInternal(ULocalCharacterComponent* Speaker);
};