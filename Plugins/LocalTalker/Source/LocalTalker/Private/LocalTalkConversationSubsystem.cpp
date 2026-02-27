#include "LocalTalkConversationSubsystem.h"
#include "LocalCharacterComponent.h"
#include "LocalTalkerSettings.h"
#include "LocalTalkerLog.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"

static FString LocalTalkerTimePrefixSubsystem(const UObject* Obj)
{
    const UWorld* W = Obj ? Obj->GetWorld() : nullptr;
    if (!W)
    {
        return TEXT("");
    }
    return FString::Printf(TEXT("[t=%.2f] "), W->GetTimeSeconds());
}

static bool LocalTalkerIsAnyPlayerPawnInHearingRange(const UWorld* World, const FLocalConversationContext& Context)
{
    if (!World) return false;
    if (Context.Participants.Num() == 0) return false;

    for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
    {
        const APlayerController* PC = It->Get();
        if (!PC) continue;
        const APawn* Pawn = PC->GetPawn();
        if (!Pawn) continue;

        const FVector PawnLoc = Pawn->GetActorLocation();

        // "In range to hear" = within any participant's hearing radius.
        for (const auto& WeakP : Context.Participants)
        {
            const ULocalCharacterComponent* P = WeakP.Get();
            if (!P || !P->GetOwner()) continue;
            const float R = FMath::Max(0.0f, P->GetHearingRadius());
            if (R <= 0.0f) continue;

            const FVector TalkerLoc = P->GetOwner()->GetActorLocation();
            if (FVector::DistSquared(PawnLoc, TalkerLoc) <= (R * R))
            {
                return true;
            }
        }
    }

    return false;
}

void ULocalTalkConversationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    UE_LOG(LogLocalTalker, Log, TEXT("%s[Director] Conversation Subsystem Initialized."), *LocalTalkerTimePrefixSubsystem(this));
}

TStatId ULocalTalkConversationSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(ULocalTalkConversationSubsystem, STATGROUP_Tickables);
}

void ULocalTalkConversationSubsystem::Tick(float DeltaTime)
{
    // Cleanup invalid actors from registry
    Registry.Remove(nullptr);
    
    UpdateContexts();
    ProcessTurns();
    MaintainKeepAlive();
}

void ULocalTalkConversationSubsystem::RegisterTalker(ULocalCharacterComponent* Talker)
{
    if (!Talker) return;
    Registry.Add(Talker);
    UE_LOG(LogLocalTalker, Log, TEXT("%s[Director] Registered: '%s' (Total: %d)"),
        *LocalTalkerTimePrefixSubsystem(this),
        *Talker->GetSpeakerNameResolved(), Registry.Num());
}

void ULocalTalkConversationSubsystem::UnregisterTalker(ULocalCharacterComponent* Talker)
{
    if (!Talker) return;
    
    FString Name = Talker->GetSpeakerNameResolved();
    Registry.Remove(Talker);
    
    // Remove from manual queue if they were waiting
    ManualQueue.RemoveAll([Talker](const FQueuedTurn& T) { return T.Talker.Get() == Talker; });

    UE_LOG(LogLocalTalker, Log, TEXT("%s[Director] Unregistered: '%s'"), *LocalTalkerTimePrefixSubsystem(this), *Name);
}

void ULocalTalkConversationSubsystem::RequestTurn(ULocalCharacterComponent* Talker, const FString& Prompt)
{
    if (!Talker || Prompt.IsEmpty()) return;

    const UWorld* W = GetWorld();
    const double Now = W ? (double)W->GetTimeSeconds() : 0.0;

    // If it's a user prompt (not RAW and not a Director/internal instruction), add to history.
    if (!Prompt.StartsWith(TEXT("RAW:")) &&
        !Prompt.StartsWith(TEXT("Respond to ")) &&
        !Prompt.StartsWith(TEXT("Director instruction:"), ESearchCase::IgnoreCase) &&
        !Prompt.StartsWith(TEXT("Instruction:"), ESearchCase::IgnoreCase))
    {
        if (FLocalConversationContext* Context = FindOrCreateContext(Talker))
        {
            AddMessageToContext(*Context, TEXT("User"), Prompt, true);
        }
    }

    EnqueueTurn(Talker, Prompt, Now);
}

void ULocalTalkConversationSubsystem::EnqueueTurn(ULocalCharacterComponent* Talker, const FString& Prompt, double EarliestGrantWorldSeconds)
{
    if (!Talker || Prompt.IsEmpty()) return;

    // Check if they are already in the queue - if so, update their prompt
    for (FQueuedTurn& Q : ManualQueue)
    {
        if (Q.Talker.Get() == Talker)
        {
            Q.Prompt = Prompt;
            // Preserve the later of the existing earliest time and the new one.
            Q.EarliestGrantWorldSeconds = FMath::Max(Q.EarliestGrantWorldSeconds, EarliestGrantWorldSeconds);
            return;
        }
    }

    FQueuedTurn NewTurn;
    NewTurn.Talker = Talker;
    NewTurn.Prompt = Prompt;
    NewTurn.EarliestGrantWorldSeconds = EarliestGrantWorldSeconds;
    ManualQueue.Add(MoveTemp(NewTurn));
    
    UE_LOG(LogLocalTalker, Log, TEXT("%s[Director] '%s' requested turn (Manual Queue length: %d)"),
        *LocalTalkerTimePrefixSubsystem(this),
        *Talker->GetSpeakerNameResolved(), ManualQueue.Num());
}

void ULocalTalkConversationSubsystem::ReleaseTurn(ULocalCharacterComponent* Talker)
{
    if (!Talker) return;
    
    UE_LOG(LogLocalTalker, Log, TEXT("%s[Director] '%s' finished talking."),
        *LocalTalkerTimePrefixSubsystem(this),
        *Talker->GetSpeakerNameResolved());

    // If the talker didn't emit any speech this turn, do NOT auto-trigger a response.
    // This prevents infinite ping-pong when an LLM/TTS path fails and characters immediately "finish".
    if (!Talker->bSpokeThisTurn)
    {
        return;
    }
    
    // When someone finished, evaluate if someone else in their group should respond
    if (FLocalConversationContext* Context = FindOrCreateContext(Talker))
    {
        EvaluateNextSpeaker(*Context, Talker);
    }
}

void ULocalTalkConversationSubsystem::BroadcastSentence(ULocalCharacterComponent* Speaker, const FString& Text, bool bFromUser)
{
    if (!Speaker || Text.IsEmpty()) return;

    FLocalConversationContext* Context = FindOrCreateContext(Speaker);
    if (!Context) return;

    FString SpeakerName = Speaker->GetSpeakerNameResolved();
    UE_LOG(LogLocalTalker, Log, TEXT("%s[Director] %s: %s"), *LocalTalkerTimePrefixSubsystem(this), *SpeakerName, *Text);

    AddMessageToContext(*Context, SpeakerName, Text, bFromUser);

    // Notify listeners in the context
    for (auto& WeakParticipant : Context->Participants)
    {
        ULocalCharacterComponent* Listener = WeakParticipant.Get();
        if (Listener && Listener != Speaker)
        {
            Listener->OnHeardSpeech(SpeakerName, Text, bFromUser);
        }
    }
}

TArray<FLocalTalkMessage> ULocalTalkConversationSubsystem::GetContextHistory(ULocalCharacterComponent* Agent)
{
    if (FLocalConversationContext* Context = FindOrCreateContext(Agent))
    {
        return Context->History;
    }
    return TArray<FLocalTalkMessage>();
}

TArray<ULocalCharacterComponent*> ULocalTalkConversationSubsystem::GetContextParticipants(const ULocalCharacterComponent* Agent) const
{
    TArray<ULocalCharacterComponent*> Out;
    if (FLocalConversationContext* Context = const_cast<ULocalTalkConversationSubsystem*>(this)->FindOrCreateContext(const_cast<ULocalCharacterComponent*>(Agent)))
    {
        for (const TWeakObjectPtr<ULocalCharacterComponent>& Weak : Context->Participants)
        {
            if (ULocalCharacterComponent* P = Weak.Get())
            {
                Out.Add(P);
            }
        }
    }
    return Out;
}

void ULocalTalkConversationSubsystem::ClearContextHistory(ULocalCharacterComponent* Agent)
{
    if (FLocalConversationContext* Context = FindOrCreateContext(Agent))
    {
        Context->History.Reset();
        Context->ConsecutiveNpcTurns = 0;
        Context->LastInteractionTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
    }
}

void ULocalTalkConversationSubsystem::NotifyAudioFinished(ULocalCharacterComponent* Talker)
{
    if (!Talker) return;
    FLocalConversationContext* Context = FindOrCreateContext(Talker);
    if (!Context) return;

    RefreshContextParticipants(*Context);
    for (const auto& Weak : Context->Participants)
    {
        if (ULocalCharacterComponent* P = Weak.Get())
        {
            P->TryStartPendingAudio();
        }
    }
}

void ULocalTalkConversationSubsystem::UpdateContexts()
{
    UWorld* W = GetWorld();
    if (!W) return;
    float CurrentTime = W->GetTimeSeconds();

    // 1. Cleanup old contexts (inactive for > 30s)
    const ULocalTalkerSettings* S = GetDefault<ULocalTalkerSettings>();
    const bool bKeepAlive = S ? S->bKeepConversationAlive : false;
    const float CleanupSeconds = S ? S->ContextCleanupSeconds : 30.0f;
    if (!bKeepAlive)
    {
        ActiveContexts.RemoveAll([CurrentTime, CleanupSeconds](const FLocalConversationContext& C) {
            return (CleanupSeconds > 0.0f) && ((CurrentTime - C.LastInteractionTime) > CleanupSeconds);
        });
    }

    // 2. Refresh participants for each context based on proximity to context center
    for (auto& Context : ActiveContexts)
    {
        RefreshContextParticipants(Context);
    }
}

void ULocalTalkConversationSubsystem::MaintainKeepAlive()
{
    UWorld* W = GetWorld();
    if (!W) return;

    const ULocalTalkerSettings* S = GetDefault<ULocalTalkerSettings>();
    if (!S || !S->bKeepConversationAlive) return;

    const bool bRequireListenerAll = S->bRequirePlayerListenerForAllTalk;
    const float MaxSilence = FMath::Max(0.0f, S->MaxSilenceSeconds);
    const float MinDelay = FMath::Max(0.0f, S->MinSecondsBetweenAutoReplies);
    const bool bAllowNpcToNpc = S->bAllowNpcToNpcAuto;
    const bool bRequireListener = S->bRequirePlayerListenerForAuto;
    const bool bIgnoreListener = S->bKeepAliveIgnoresPlayerListenerRequirement;

    if (!bAllowNpcToNpc) return;

    const float Now = W->GetTimeSeconds();

    for (FLocalConversationContext& Context : ActiveContexts)
    {
        if (Context.Participants.Num() == 0) continue;

        // Allow one prewarm: block if anyone is generating or if more than one participant has pending audio.
        int32 NumGenerating = 0;
        int32 NumPendingAudio = 0;
        for (const auto& Weak : Context.Participants)
        {
            if (ULocalCharacterComponent* P = Weak.Get())
            {
                if (P->IsGenerationBusy()) NumGenerating++;
                if (!P->IsAudioPlaybackComplete()) NumPendingAudio++;
            }
        }
        if (NumGenerating > 0 || NumPendingAudio > 1) continue;

        // Respect listener gating unless explicitly ignored for keep-alive.
        if (bRequireListener && !bIgnoreListener && !LocalTalkerIsAnyPlayerPawnInHearingRange(W, Context))
        {
            continue;
        }

        if (bRequireListenerAll && !LocalTalkerIsAnyPlayerPawnInHearingRange(W, Context))
        {
            continue;
        }

        const float Silence = Now - Context.LastInteractionTime;
        if (Silence < MaxSilence) continue;

        // Avoid enqueue spam: only enqueue once per silence window (plus a small buffer).
        if (Context.LastAutoEnqueueTime > 0.0f && (Now - Context.LastAutoEnqueueTime) < FMath::Max(0.2f, MaxSilence * 0.5f))
        {
            continue;
        }

        // If any queued turn already exists for someone in this context, let it play out.
        bool bAlreadyQueued = false;
        for (const FQueuedTurn& Q : ManualQueue)
        {
            ULocalCharacterComponent* QT = Q.Talker.Get();
            if (!QT) continue;
            for (const auto& Weak : Context.Participants)
            {
                if (Weak.Get() == QT)
                {
                    bAlreadyQueued = true;
                    break;
                }
            }
            if (bAlreadyQueued) break;
        }
        if (bAlreadyQueued) continue;

        // Pick a speaker (simple: first valid participant).
        ULocalCharacterComponent* Candidate = nullptr;
        for (const auto& Weak : Context.Participants)
        {
            if (ULocalCharacterComponent* P = Weak.Get())
            {
                if (P->IsAudioPlaybackComplete())
                {
                    Candidate = P;
                    break;
                }
            }
        }
        if (!Candidate) continue;

        FString Prompt;
        if (Context.History.Num() > 0)
        {
            const FLocalTalkMessage& LastMsg = Context.History.Last();
            Prompt = FString::Printf(
                TEXT("Director instruction: Keep the conversation alive. Respond in-character to %s, add a NEW concrete detail or viewpoint, avoid repeating their wording, and end with a fresh question."),
                *LastMsg.SpeakerName
            );
        }
        else
        {
            Prompt = TEXT("Director instruction: Start a natural in-character conversation with the nearby person. Say something specific and end with a question.");
        }

        // Enforce pacing via the queued turn's earliest-grant time.
        const double Earliest = (double)Now + (double)MinDelay;
        EnqueueTurn(Candidate, Prompt, Earliest);
        Context.LastAutoEnqueueTime = Now;
    }
}

FLocalConversationContext* ULocalTalkConversationSubsystem::FindOrCreateContext(ULocalCharacterComponent* Agent)
{
    if (!Agent || !Agent->GetOwner()) return nullptr;
    FVector Loc = Agent->GetOwner()->GetActorLocation();

    // Try to find an existing context near this location
    for (auto& Context : ActiveContexts)
    {
        if (FVector::Dist(Context.LastCenter, Loc) < Agent->GetHearingRadius())
        {
            // If we found one, update its center to reflect ongoing activity
            Context.LastCenter = FMath::Lerp(Context.LastCenter, Loc, 0.2f);
            return &Context;
        }
    }

    // Create new context
    FLocalConversationContext NewContext;
    NewContext.LastCenter = Loc;
    NewContext.LastInteractionTime = GetWorld()->GetTimeSeconds();
    NewContext.Participants.Add(Agent);
    int32 Index = ActiveContexts.Add(NewContext);
    RefreshContextParticipants(ActiveContexts[Index]);
    return &ActiveContexts[Index];
}

void ULocalTalkConversationSubsystem::AddMessageToContext(FLocalConversationContext& Context, const FString& Speaker, const FString& Text, bool bFromUser)
{
    if (Context.History.Num() > 0)
    {
        const FLocalTalkMessage& Last = Context.History.Last();
        if (Last.bFromUser == bFromUser &&
            Last.SpeakerName.Equals(Speaker, ESearchCase::IgnoreCase) &&
            Last.Content.Equals(Text, ESearchCase::IgnoreCase))
        {
            Context.LastInteractionTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
            return;
        }
    }

    Context.History.Add({ Speaker, Text, bFromUser });
    // Keep a slightly longer rolling window of messages so the LLM can
    // see more prior turns when building prompts.
    const int32 MaxHistoryMessages = 16;
    if (Context.History.Num() > MaxHistoryMessages)
    {
        Context.History.RemoveAt(0);
    }
    Context.LastInteractionTime = GetWorld()->GetTimeSeconds();

    if (bFromUser)
    {
        Context.ConsecutiveNpcTurns = 0;
    }
    else
    {
        Context.ConsecutiveNpcTurns++;
    }
}

void ULocalTalkConversationSubsystem::EvaluateNextSpeaker(FLocalConversationContext& Context, ULocalCharacterComponent* LastSpeaker)
{
    RefreshContextParticipants(Context);

    const ULocalTalkerSettings* S = GetDefault<ULocalTalkerSettings>();

    // Allow one prewarm: block if anyone is generating or if more than one participant has pending audio.
    int32 NumGenerating = 0;
    int32 NumPendingAudio = 0;
    for (auto& Weak : Context.Participants)
    {
        if (ULocalCharacterComponent* P = Weak.Get())
        {
            if (P->IsGenerationBusy()) NumGenerating++;
            if (!P->IsAudioPlaybackComplete()) NumPendingAudio++;
        }
    }
    if (NumGenerating > 0 || NumPendingAudio > 1)
    {
        return;
    }

    if (Context.History.Num() <= 0)
    {
        return;
    }

    const FLocalTalkMessage& LastMsg = Context.History.Last();

    const bool bAllowNpcToNpc = S ? S->bAllowNpcToNpcAuto : false;
    const int32 MaxNpcTurns = S ? S->MaxConsecutiveNpcTurns : 0;
    const float MinDelay = S ? S->MinSecondsBetweenAutoReplies : 0.0f;
    const bool bRequireListener = S ? S->bRequirePlayerListenerForAuto : false;
    const bool bKeepAlive = S ? S->bKeepConversationAlive : false;
    const bool bIgnoreListener = S ? S->bKeepAliveIgnoresPlayerListenerRequirement : false;

    // Default rule: auto-respond to USER messages. Optionally allow NPC-to-NPC within a capped streak.
    if (!LastMsg.bFromUser)
    {
        if (!bAllowNpcToNpc)
        {
            return;
        }
        if (bRequireListener && !(bKeepAlive && bIgnoreListener) && !LocalTalkerIsAnyPlayerPawnInHearingRange(GetWorld(), Context))
        {
            return;
        }
        if (MaxNpcTurns > 0 && Context.ConsecutiveNpcTurns > MaxNpcTurns)
        {
            return;
        }
    }

    // Logic: Pick someone other than the last speaker to respond
    // In a more complex version, this could use priority, personality, etc.
    bool bFoundCandidate = false;
    for (auto& Weak : Context.Participants)
    {
        ULocalCharacterComponent* Candidate = Weak.Get();
        if (Candidate && Candidate != LastSpeaker && !Candidate->IsGenerationBusy() && Candidate->IsAudioPlaybackComplete())
        {
            // Only respond if the last message was within a reasonable timeframe
            const float TimeSinceLast = GetWorld()->GetTimeSeconds() - Context.LastInteractionTime;
            if (TimeSinceLast < 5.0f)
            {
                const FString Prompt = FString::Printf(
                    TEXT("Director instruction: Respond in-character to %s. Add a NEW detail or viewpoint, do not echo their exact wording, and end with a natural follow-up question."),
                    *LastMsg.SpeakerName
                );
                UE_LOG(LogLocalTalker, Log, TEXT("%s[Director] -> TRIGGERING RESPONSE from '%s'"),
                    *LocalTalkerTimePrefixSubsystem(this),
                    *Candidate->GetSpeakerNameResolved());
                // Queue as a normal turn, but delay granting so conversations don't machine-gun between NPCs.
                const double Now = (double)GetWorld()->GetTimeSeconds();
                const double Earliest = Now + (double)FMath::Max(0.0f, MinDelay);
                EnqueueTurn(Candidate, Prompt, Earliest);
                bFoundCandidate = true;
                return;
            }
        }
    }

    // Solo continuation: if only one participant exists and a player can hear, let them keep talking.
    if (!bFoundCandidate &&
        LastSpeaker &&
        Context.Participants.Num() == 1 &&
        Context.Participants[0].Get() == LastSpeaker &&
        !LastMsg.bFromUser &&
        bAllowNpcToNpc)
    {
        if (LastSpeaker->IsAudioPlaybackComplete() &&
            (!bRequireListener || LocalTalkerIsAnyPlayerPawnInHearingRange(GetWorld(), Context)))
        {
            if (!(MaxNpcTurns > 0 && Context.ConsecutiveNpcTurns > MaxNpcTurns))
            {
                const float TimeSinceLast = GetWorld()->GetTimeSeconds() - Context.LastInteractionTime;
                if (TimeSinceLast < 5.0f)
                {
                    const FString Prompt = FString::Printf(
                        TEXT("Director instruction: Continue speaking to the nearby listener. Add a NEW detail or viewpoint, do not echo your exact wording, and end with a natural question.")
                    );
                    UE_LOG(LogLocalTalker, Log, TEXT("%s[Director] -> TRIGGERING SOLO CONTINUATION from '%s'"),
                        *LocalTalkerTimePrefixSubsystem(this),
                        *LastSpeaker->GetSpeakerNameResolved());
                    const double Now = (double)GetWorld()->GetTimeSeconds();
                    const double Earliest = Now + (double)FMath::Max(0.0f, MinDelay);
                    EnqueueTurn(LastSpeaker, Prompt, Earliest);
                }
            }
        }
    }
}

void ULocalTalkConversationSubsystem::RefreshContextParticipants(FLocalConversationContext& Context)
{
    Context.Participants.Empty();
    for (auto& WeakAgent : Registry)
    {
        ULocalCharacterComponent* Agent = WeakAgent.Get();
        if (!Agent || !Agent->GetOwner()) continue;

        const float R = FMath::Max(0.0f, Agent->GetHearingRadius());
        if (R <= 0.0f) continue;

        const float Dist = FVector::Dist(Agent->GetOwner()->GetActorLocation(), Context.LastCenter);
        if (Dist <= R)
        {
            Context.Participants.Add(Agent);
        }
    }
}

void ULocalTalkConversationSubsystem::ProcessTurns()
{
    const UWorld* W = GetWorld();
    const double Now = W ? (double)W->GetTimeSeconds() : 0.0;
    const ULocalTalkerSettings* S = GetDefault<ULocalTalkerSettings>();
    const bool bRequireListenerAll = S ? S->bRequirePlayerListenerForAllTalk : false;

    // Handle manual requests (e.g. Player interaction or scripted events)
    for (int32 i = 0; i < ManualQueue.Num(); i++)
    {
        ULocalCharacterComponent* T = ManualQueue[i].Talker.Get();
        if (!T)
        {
            ManualQueue.RemoveAt(i);
            i--;
            continue;
        }

        if (Now < ManualQueue[i].EarliestGrantWorldSeconds)
        {
            continue;
        }

        if (T->IsBusy()) continue;

        FLocalConversationContext* Context = FindOrCreateContext(T);
        if (bRequireListenerAll && Context && !LocalTalkerIsAnyPlayerPawnInHearingRange(GetWorld(), *Context))
        {
            continue;
        }

        // Allow one "pre-warm" generation while someone else is speaking:
        // - Block if any participant is currently generating.
        // - Block if more than one participant already has pending audio playback.
        bool bContextBusy = false;
        if (Context)
        {
            int32 NumGenerating = 0;
            int32 NumPendingAudio = 0;
            for (auto& Weak : Context->Participants)
            {
                if (ULocalCharacterComponent* P = Weak.Get())
                {
                    if (P->IsGenerationBusy()) NumGenerating++;
                    if (!P->IsAudioPlaybackComplete()) NumPendingAudio++;
                }
            }

            if (NumGenerating > 0)
            {
                bContextBusy = true;
            }
            else if (NumPendingAudio > 1)
            {
                bContextBusy = true;
            }
        }

        if (!bContextBusy)
        {
            FString Prompt = ManualQueue[i].Prompt;
            ManualQueue.RemoveAt(i);
            i--;

            UE_LOG(LogLocalTalker, Log, TEXT("%s[Director] -> GRANTING TURN to: '%s'"),
                *LocalTalkerTimePrefixSubsystem(this),
                *T->GetSpeakerNameResolved());
            T->InternalGrantTurn(Prompt);
        }
    }
}

void ULocalTalkConversationSubsystem::InterruptProximity(const FVector& Location, float Radius)
{
    const float RadiusSq = Radius * Radius;
    for (const TWeakObjectPtr<ULocalCharacterComponent>& Weak : Registry)
    {
        ULocalCharacterComponent* T = Weak.Get();
        if (!T || !T->GetOwner()) continue;

        if (FVector::DistSquared(T->GetOwner()->GetActorLocation(), Location) <= RadiusSq)
        {
            T->Interrupt();
        }
    }
}

TArray<ULocalCharacterComponent*> ULocalTalkConversationSubsystem::GetRegisteredTalkers() const
{
    TArray<ULocalCharacterComponent*> Result;
    for (const TWeakObjectPtr<ULocalCharacterComponent>& Weak : Registry)
    {
        if (Weak.IsValid()) Result.Add(Weak.Get());
    }
    return Result;
}

bool ULocalTalkConversationSubsystem::HasPlayerListenerInRange(const ULocalCharacterComponent* Talker) const
{
    if (!Talker) return false;
    const UWorld* W = GetWorld();
    if (!W) return false;

    FLocalConversationContext* Context = const_cast<ULocalTalkConversationSubsystem*>(this)
        ->FindOrCreateContext(const_cast<ULocalCharacterComponent*>(Talker));
    if (!Context) return false;

    return LocalTalkerIsAnyPlayerPawnInHearingRange(W, *Context);
}
