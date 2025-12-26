#include "LocalTalkConversationSubsystem.h"

#include "LocalCharacterComponent.h"
#include "LocalTalkerLog.h"

#include "Engine/World.h"

static double NowSeconds()
{
    return FPlatformTime::Seconds();
}

void ULocalTalkConversationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    const double Now = NowSeconds();
    LastConversationActivitySeconds = Now;
    RescheduleIdleChatter(Now);
}

void ULocalTalkConversationSubsystem::Deinitialize()
{
    Talkers.Reset();
    TalkerStates.Reset();
    AggregatedBySpeaker.Reset();
    PendingUtterances.Reset();

    Super::Deinitialize();
}

TStatId ULocalTalkConversationSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(ULocalTalkConversationSubsystem, STATGROUP_Tickables);
}

void ULocalTalkConversationSubsystem::Tick(float DeltaTime)
{
    Compact();
    UpdateTalkerStates();
    ProcessPendingUtterances();
    TryStartIdleChatter();
}

void ULocalTalkConversationSubsystem::RegisterTalker(ULocalCharacterComponent* Talker)
{
    if (!Talker) return;
    Talkers.Add(Talker);
    GetOrCreateTalkerState(Talker);
}

void ULocalTalkConversationSubsystem::UnregisterTalker(ULocalCharacterComponent* Talker)
{
    if (!Talker) return;
    Talkers.Remove(Talker);
    TalkerStates.Remove(Talker);
    AggregatedBySpeaker.Remove(Talker);

    PendingUtterances.RemoveAll([Talker](const FPendingUtterance& U)
    {
        return U.SelectedResponder.Get() == Talker;
    });
}

ULocalTalkConversationSubsystem::FTalkerState& ULocalTalkConversationSubsystem::GetOrCreateTalkerState(ULocalCharacterComponent* Talker)
{
    if (FTalkerState* Existing = TalkerStates.Find(Talker))
    {
        return *Existing;
    }

    FTalkerState NewState;
    NewState.State = ETalkerState::Idle;
    NewState.StateStartTime = NowSeconds();
    TalkerStates.Add(Talker, NewState);
    return TalkerStates.FindChecked(Talker);
}

void ULocalTalkConversationSubsystem::SetTalkerState(ULocalCharacterComponent* Talker, ETalkerState NewState)
{
    if (!Talker) return;
    FTalkerState& S = GetOrCreateTalkerState(Talker);
    S.State = NewState;
    S.StateStartTime = NowSeconds();
}

void ULocalTalkConversationSubsystem::NotifyStartedSpeaking(ULocalCharacterComponent* Speaker)
{
    if (!Speaker) return;
    SetTalkerState(Speaker, ETalkerState::Generating);
}

void ULocalTalkConversationSubsystem::NotifySentenceSpoken(ULocalCharacterComponent* Speaker, const FString& Sentence, bool bFromUser)
{
    if (!Speaker) return;
    if (Sentence.IsEmpty()) return;

    FString& Accum = AggregatedBySpeaker.FindOrAdd(Speaker);
    if (!Accum.IsEmpty()) Accum += TEXT(" ");
    Accum += Sentence;

    // Broadcast live context to nearby listeners.
    const FVector SpeakerLoc = Speaker->GetOwner() ? Speaker->GetOwner()->GetActorLocation() : FVector::ZeroVector;
    const float Radius = Speaker->ConversationRadius;
    const float RadiusSq = Radius * Radius;
    const FString SpeakerName = Speaker->GetResolvedSpeakerName();

    for (const TWeakObjectPtr<ULocalCharacterComponent>& Weak : Talkers)
    {
        ULocalCharacterComponent* Listener = Weak.Get();
        if (!Listener || Listener == Speaker) continue;
        if (!Listener->bEnableProximityConversation) continue;

        const AActor* LOwner = Listener->GetOwner();
        if (!LOwner) continue;

        const float DistSq = FVector::DistSquared(LOwner->GetActorLocation(), SpeakerLoc);
        if (DistSq > RadiusSq) continue;

        Listener->ReceiveBroadcastSpeech(SpeakerName, Sentence, bFromUser);
    }
}

void ULocalTalkConversationSubsystem::NotifyFinishedSpeaking(ULocalCharacterComponent* Speaker)
{
    if (!Speaker) return;
    SetTalkerState(Speaker, ETalkerState::PlayingAudio);
}

void ULocalTalkConversationSubsystem::NotifyAudioPlaybackFinished(ULocalCharacterComponent* Speaker)
{
    if (!Speaker) return;

    FTalkerState& S = GetOrCreateTalkerState(Speaker);
    S.State = ETalkerState::Idle;
    S.StateStartTime = NowSeconds();
    S.LastSpokeTime = NowSeconds();
    S.RecentTurnCount++;

    FString HeardText;
    if (FString* Accum = AggregatedBySpeaker.Find(Speaker))
    {
        HeardText = *Accum;
    }
    AggregatedBySpeaker.Remove(Speaker);
    HeardText.TrimStartAndEndInline();
    if (HeardText.IsEmpty()) return;

    LastConversationActivitySeconds = NowSeconds();
    RescheduleIdleChatter(LastConversationActivitySeconds);

    const FVector SpeakerLoc = Speaker->GetOwner() ? Speaker->GetOwner()->GetActorLocation() : FVector::ZeroVector;
    const float Radius = Speaker->ConversationRadius;
    const FString SpeakerName = Speaker->GetResolvedSpeakerName();

    // Queue a single "utterance" that will get at most one response.
    const float ThinkDelay = FMath::FRandRange(Speaker->ThinkingDelayMin, Speaker->ThinkingDelayMax);

    FPendingUtterance Utterance;
    Utterance.SpeakerName = SpeakerName;
    Utterance.Text = HeardText;
    Utterance.Location = SpeakerLoc;
    Utterance.Radius = Radius;
    Utterance.TimeReceived = NowSeconds();
    Utterance.ResponseDueTime = NowSeconds() + ThinkDelay;
    Utterance.bFromUser = false;
    Utterance.bResponderSelected = false;

    PendingUtterances.Add(MoveTemp(Utterance));
}

void ULocalTalkConversationSubsystem::NotifyInterrupted(ULocalCharacterComponent* Speaker)
{
    CancelSpeakingInternal(Speaker);
}

void ULocalTalkConversationSubsystem::BroadcastUserUtterance(const FVector& Location, float Radius, const FString& UserText, bool bInterrupt)
{
    if (UserText.IsEmpty()) return;
    if (Radius <= 0.0f) return;

    LastConversationActivitySeconds = NowSeconds();
    RescheduleIdleChatter(LastConversationActivitySeconds);

    const float RadiusSq = Radius * Radius;

    struct FCand { ULocalCharacterComponent* Talker = nullptr; float DistSq = 0.0f; };
    TArray<FCand> Cands;

    for (const TWeakObjectPtr<ULocalCharacterComponent>& Weak : Talkers)
    {
        ULocalCharacterComponent* T = Weak.Get();
        if (!T || !T->bEnableProximityConversation) continue;
        const AActor* Owner = T->GetOwner();
        if (!Owner) continue;

        const float DistSq = FVector::DistSquared(Owner->GetActorLocation(), Location);
        if (DistSq > RadiusSq) continue;

        Cands.Add({ T, DistSq });
    }

    if (Cands.Num() == 0) return;
    Cands.Sort([](const FCand& A, const FCand& B) { return A.DistSq < B.DistSq; });

    // Only player/user input may interrupt.
    if (bInterrupt)
    {
        for (const FCand& C : Cands)
        {
            if (!C.Talker) continue;
            C.Talker->Interrupt();
            CancelSpeakingInternal(C.Talker);
        }
    }

    // Everyone hears the user for context.
    for (const FCand& C : Cands)
    {
        if (!C.Talker) continue;
        C.Talker->ReceiveBroadcastSpeech(TEXT("User"), UserText, /*bFromUser*/ true);
    }

    // Queue an utterance that MUST get a response.
    FPendingUtterance Utterance;
    Utterance.SpeakerName = TEXT("User");
    Utterance.Text = UserText;
    Utterance.Location = Location;
    Utterance.Radius = Radius;
    Utterance.TimeReceived = NowSeconds();
    Utterance.ResponseDueTime = NowSeconds() + FMath::FRandRange(0.2f, 0.6f);
    Utterance.bFromUser = true;
    Utterance.bResponderSelected = false;

    PendingUtterances.Add(MoveTemp(Utterance));
}

bool ULocalTalkConversationSubsystem::IsAnyoneSpeakingNear(const FVector& Location, float Radius) const
{
    const float RadiusSq = Radius * Radius;

    for (const auto& Pair : TalkerStates)
    {
        ULocalCharacterComponent* Talker = Pair.Key.Get();
        if (!Talker) continue;

        const FTalkerState& S = Pair.Value;
        if (S.State == ETalkerState::Idle) continue;

        const AActor* Owner = Talker->GetOwner();
        if (!Owner) continue;

        if (FVector::DistSquared(Owner->GetActorLocation(), Location) <= RadiusSq)
        {
            return true;
        }
    }
    return false;
}

bool ULocalTalkConversationSubsystem::IsSpeaking(ULocalCharacterComponent* Talker) const
{
    if (!Talker) return false;
    const FTalkerState* S = TalkerStates.Find(Talker);
    return S && S->State != ETalkerState::Idle;
}

void ULocalTalkConversationSubsystem::Compact()
{
    Talkers.Remove(nullptr);
    TalkerStates.Remove(nullptr);
    AggregatedBySpeaker.Remove(nullptr);

    const double Now = NowSeconds();
    PendingUtterances.RemoveAll([Now](const FPendingUtterance& U)
    {
        // Don't keep ancient pending utterances around forever.
        return (Now - U.TimeReceived) > 60.0;
    });
}

void ULocalTalkConversationSubsystem::UpdateTalkerStates()
{
    const double Now = NowSeconds();

    for (auto& Pair : TalkerStates)
    {
        FTalkerState& S = Pair.Value;
        if (S.LastSpokeTime > 0.0 && (Now - S.LastSpokeTime) > RecentTurnWindow)
        {
            S.RecentTurnCount = FMath::Max(0, S.RecentTurnCount - 1);
        }
    }
}

void ULocalTalkConversationSubsystem::ProcessPendingUtterances()
{
    const double Now = NowSeconds();

    for (int32 i = PendingUtterances.Num() - 1; i >= 0; i--)
    {
        FPendingUtterance& Utterance = PendingUtterances[i];

        if (Utterance.bResponderSelected)
        {
            ULocalCharacterComponent* Responder = Utterance.SelectedResponder.Get();
            if (!Responder)
            {
                PendingUtterances.RemoveAt(i);
                continue;
            }

            if (IsAnyoneSpeakingNear(Utterance.Location, Utterance.Radius))
            {
                continue;
            }

            if (!CanRespond(Responder))
            {
                continue;
            }

            DispatchResponse(Responder, Utterance);
            PendingUtterances.RemoveAt(i);
            continue;
        }

        if (Now < Utterance.ResponseDueTime)
        {
            continue;
        }

        if (IsAnyoneSpeakingNear(Utterance.Location, Utterance.Radius))
        {
            Utterance.ResponseDueTime = Now + 0.35;
            continue;
        }

        SelectResponderForUtterance(Utterance);
    }
}

void ULocalTalkConversationSubsystem::SelectResponderForUtterance(FPendingUtterance& Utterance)
{
    const double Now = NowSeconds();
    const float RadiusSq = Utterance.Radius * Utterance.Radius;

    struct FCandidate
    {
        ULocalCharacterComponent* Talker = nullptr;
        float Priority = 0.0f;
        float DistSq = 0.0f;
    };
    TArray<FCandidate> Candidates;

    for (const TWeakObjectPtr<ULocalCharacterComponent>& Weak : Talkers)
    {
        ULocalCharacterComponent* Talker = Weak.Get();
        if (!Talker) continue;
        if (!Talker->bEnableProximityConversation) continue;
        if (!Talker->bAutoRespondToHeardSpeech) continue;

        if (Talker->GetResolvedSpeakerName() == Utterance.SpeakerName) continue;

        const AActor* Owner = Talker->GetOwner();
        if (!Owner) continue;

        const float DistSq = FVector::DistSquared(Owner->GetActorLocation(), Utterance.Location);
        if (DistSq > RadiusSq) continue;

        if (!CanRespond(Talker)) continue;

        const float Priority = CalculateResponsePriority(Talker, Utterance);
        if (Priority <= 0.0f) continue;

        Candidates.Add({ Talker, Priority, DistSq });
    }

    if (Candidates.Num() == 0)
    {
        if (Utterance.bFromUser)
        {
            // ALWAYS respond to the user: keep retrying until someone becomes available.
            Utterance.ResponseDueTime = Now + 0.25;
            Utterance.bResponderSelected = false;
            return;
        }

        // NPC utterance can be dropped if nobody is able/willing to respond.
        Utterance.bResponderSelected = true;
        Utterance.SelectedResponder = nullptr;
        return;
    }

    // Sort by priority desc; tie-break by distance asc.
    Candidates.Sort([](const FCandidate& A, const FCandidate& B)
    {
        if (A.Priority != B.Priority) return A.Priority > B.Priority;
        return A.DistSq < B.DistSq;
    });

    // For user utterances: pick best candidate deterministically (closest/highest priority).
    // For NPC utterances: small randomization so it doesn't feel rigid.
    int32 SelectedIdx = 0;
    if (!Utterance.bFromUser && Candidates.Num() > 1 && FMath::FRand() > 0.75f)
    {
        SelectedIdx = 1;
    }

    Utterance.bResponderSelected = true;
    Utterance.SelectedResponder = Candidates[SelectedIdx].Talker;
}

float ULocalTalkConversationSubsystem::CalculateResponsePriority(ULocalCharacterComponent* Candidate, const FPendingUtterance& Utterance) const
{
    if (!Candidate) return 0.0f;

    // NPC-to-NPC chatter gate (user utterances bypass this: ALWAYS respond).
    if (!Utterance.bFromUser)
    {
        if (FMath::FRand() > Candidate->ResponseLikelihood)
        {
            return 0.0f;
        }
    }

    float Priority = 1.0f;

    if (Utterance.bFromUser)
    {
        Priority *= Candidate->UserResponsePriorityBoost;
    }
    else
    {
        Priority *= FMath::Max(0.01f, Candidate->ResponseLikelihood);
    }

    Priority *= FMath::FRandRange(0.9f, 1.1f);

    const AActor* Owner = Candidate->GetOwner();
    if (Owner)
    {
        const float DistSq = FVector::DistSquared(Owner->GetActorLocation(), Utterance.Location);
        const float MaxDistSq = Utterance.Radius * Utterance.Radius;
        const float DistFactor = 1.0f - FMath::Clamp(DistSq / MaxDistSq, 0.0f, 1.0f);
        Priority *= (0.5f + 0.5f * DistFactor);
    }

    const FTalkerState* S = TalkerStates.Find(Candidate);
    if (S && S->RecentTurnCount > 0)
    {
        Priority *= FMath::Pow(0.7f, (float)S->RecentTurnCount);
    }

    if (S && S->LastSpokeTime > 0.0)
    {
        const double TimeSinceSpoke = NowSeconds() - S->LastSpokeTime;
        if (TimeSinceSpoke < TurnCooldownSeconds)
        {
            Priority *= (float)(TimeSinceSpoke / TurnCooldownSeconds);
        }
    }

    return Priority;
}

bool ULocalTalkConversationSubsystem::CanRespond(ULocalCharacterComponent* Talker) const
{
    if (!Talker) return false;

    const FTalkerState* S = TalkerStates.Find(Talker);
    if (S && S->State != ETalkerState::Idle) return false;

    if (Talker->IsSpeaking()) return false;

    if (S && S->LastSpokeTime > 0.0)
    {
        const double TimeSinceSpoke = NowSeconds() - S->LastSpokeTime;
        if (TimeSinceSpoke < (double)Talker->MinSecondsBetweenTurns)
        {
            return false;
        }
    }

    return true;
}

void ULocalTalkConversationSubsystem::DispatchResponse(ULocalCharacterComponent* Responder, const FPendingUtterance& Utterance)
{
    if (!Responder) return;

    const FString Prompt = Utterance.bFromUser
        ? Utterance.Text
        : FString::Printf(TEXT("Respond to what you just heard from %s:\n%s"), *Utterance.SpeakerName, *Utterance.Text);

    Responder->SendPromptAndSpeakStreamingInProc(Prompt);
}

void ULocalTalkConversationSubsystem::RescheduleIdleChatter(double Now)
{
    // Find any talker with idle chatter enabled; use their min/max as bounds (pick conservative min/max).
    float MinSilence = 999999.0f;
    float MaxSilence = 0.0f;

    for (const TWeakObjectPtr<ULocalCharacterComponent>& Weak : Talkers)
    {
        const ULocalCharacterComponent* T = Weak.Get();
        if (!T) continue;
        if (!T->bEnableIdleChatter) continue;
        MinSilence = FMath::Min(MinSilence, T->IdleChatterMinSilenceSeconds);
        MaxSilence = FMath::Max(MaxSilence, T->IdleChatterMaxSilenceSeconds);
    }

    if (MaxSilence <= 0.0f || MinSilence > MaxSilence)
    {
        NextIdleChatterDueSeconds = 0.0;
        return;
    }

    const float Delay = FMath::FRandRange(MinSilence, MaxSilence);
    NextIdleChatterDueSeconds = Now + Delay;
}

void ULocalTalkConversationSubsystem::TryStartIdleChatter()
{
    const double Now = NowSeconds();
    if (NextIdleChatterDueSeconds <= 0.0) return;
    if (Now < NextIdleChatterDueSeconds) return;

    // Only start idle chatter if the world is quiet.
    if (PendingUtterances.Num() > 0) { NextIdleChatterDueSeconds = Now + 5.0; return; }
    if (IsAnyoneSpeakingNear(FVector::ZeroVector, TNumericLimits<float>::Max())) { NextIdleChatterDueSeconds = Now + 5.0; return; }

    // Find eligible initiators (idle, enabled, and has at least one other talker nearby).
    TArray<ULocalCharacterComponent*> Eligible;

    for (const TWeakObjectPtr<ULocalCharacterComponent>& Weak : Talkers)
    {
        ULocalCharacterComponent* T = Weak.Get();
        if (!T) continue;
        if (!T->bEnableProximityConversation) continue;
        if (!T->bEnableIdleChatter) continue;
        if (!CanRespond(T)) continue;
        if (!T->GetOwner()) continue;

        const FVector Loc = T->GetOwner()->GetActorLocation();
        const float RadiusSq = T->ConversationRadius * T->ConversationRadius;

        bool bHasListener = false;
        for (const TWeakObjectPtr<ULocalCharacterComponent>& Weak2 : Talkers)
        {
            ULocalCharacterComponent* Other = Weak2.Get();
            if (!Other || Other == T) continue;
            if (!Other->bEnableProximityConversation) continue;
            if (!Other->GetOwner()) continue;
            if (FVector::DistSquared(Other->GetOwner()->GetActorLocation(), Loc) <= RadiusSq)
            {
                bHasListener = true;
                break;
            }
        }

        if (bHasListener)
        {
            Eligible.Add(T);
        }
    }

    if (Eligible.Num() == 0)
    {
        NextIdleChatterDueSeconds = Now + 10.0;
        return;
    }

    ULocalCharacterComponent* Speaker = Eligible[FMath::RandRange(0, Eligible.Num() - 1)];
    if (!Speaker) { NextIdleChatterDueSeconds = Now + 10.0; return; }

    const FString Prompt = !Speaker->IdleChatterPrompt.IsEmpty()
        ? Speaker->IdleChatterPrompt
        : TEXT("After a long silence, say one short, natural line to nearby people. Keep it brief (1 sentence). Do not ask a question unless necessary.");

    LastConversationActivitySeconds = Now;
    RescheduleIdleChatter(Now);

    Speaker->SendPromptAndSpeakStreamingInProc(Prompt);
}

void ULocalTalkConversationSubsystem::CancelSpeakingInternal(ULocalCharacterComponent* Speaker)
{
    if (!Speaker) return;
    SetTalkerState(Speaker, ETalkerState::Idle);
    AggregatedBySpeaker.Remove(Speaker);
}