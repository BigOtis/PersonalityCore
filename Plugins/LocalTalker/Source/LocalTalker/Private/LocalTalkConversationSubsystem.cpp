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

static FString LocalTalkerOneLineSubsystem(const FString& In)
{
    FString S = In;
    S.ReplaceInline(TEXT("\r"), TEXT(" "));
    S.ReplaceInline(TEXT("\n"), TEXT(" "));
    S.ReplaceInline(TEXT("\t"), TEXT(" "));
    while (S.Contains(TEXT("  ")))
    {
        S.ReplaceInline(TEXT("  "), TEXT(" "));
    }
    S.TrimStartAndEndInline();
    return S;
}

static FString LocalTalkerNormalizeForLoopCompare(const FString& In)
{
    const FString One = LocalTalkerOneLineSubsystem(In).ToLower();
    FString Out;
    Out.Reserve(One.Len());
    bool bPrevSpace = false;
    for (int32 i = 0; i < One.Len(); ++i)
    {
        const TCHAR C = One[i];
        if (FChar::IsAlnum(C))
        {
            Out.AppendChar(C);
            bPrevSpace = false;
        }
        else if (!bPrevSpace)
        {
            Out.AppendChar(TEXT(' '));
            bPrevSpace = true;
        }
    }
    Out.TrimStartAndEndInline();
    return Out;
}

static bool LocalTalkerLikelySameUtterance(const FString& AIn, const FString& BIn)
{
    const FString A = LocalTalkerNormalizeForLoopCompare(AIn);
    const FString B = LocalTalkerNormalizeForLoopCompare(BIn);
    if (A.IsEmpty() || B.IsEmpty())
    {
        return false;
    }
    if (A.Equals(B, ESearchCase::IgnoreCase))
    {
        return true;
    }

    const int32 MinLen = FMath::Min(A.Len(), B.Len());
    if (MinLen >= 40 && (A.Contains(B, ESearchCase::IgnoreCase) || B.Contains(A, ESearchCase::IgnoreCase)))
    {
        return true;
    }

    TArray<FString> At;
    TArray<FString> Bt;
    A.ParseIntoArrayWS(At);
    B.ParseIntoArrayWS(Bt);
    if (At.Num() < 5 || Bt.Num() < 5)
    {
        return false;
    }

    TSet<FString> ASet;
    TSet<FString> BSet;
    for (const FString& T : At)
    {
        if (T.Len() >= 3) ASet.Add(T);
    }
    for (const FString& T : Bt)
    {
        if (T.Len() >= 3) BSet.Add(T);
    }
    if (ASet.Num() == 0 || BSet.Num() == 0)
    {
        return false;
    }

    int32 Intersect = 0;
    for (const FString& T : ASet)
    {
        if (BSet.Contains(T))
        {
            Intersect++;
        }
    }

    const int32 Smaller = FMath::Min(ASet.Num(), BSet.Num());
    const float Overlap = (Smaller > 0) ? ((float)Intersect / (float)Smaller) : 0.0f;
    return Overlap >= 0.72f;
}

static bool LocalTalkerContextLooksRepetitive(const FLocalConversationContext& Context)
{
    if (Context.History.Num() < 4)
    {
        return false;
    }

    int32 LastUserIdx = INDEX_NONE;
    for (int32 i = Context.History.Num() - 1; i >= 0; --i)
    {
        if (Context.History[i].bFromUser)
        {
            LastUserIdx = i;
            break;
        }
    }

    TArray<const FLocalTalkMessage*> RecentNpc;
    for (int32 i = Context.History.Num() - 1; i > LastUserIdx && RecentNpc.Num() < 6; --i)
    {
        const FLocalTalkMessage& M = Context.History[i];
        if (M.bFromUser)
        {
            continue;
        }
        if (LocalTalkerOneLineSubsystem(M.Content).IsEmpty())
        {
            continue;
        }
        RecentNpc.Add(&M);
    }

    if (RecentNpc.Num() < 4)
    {
        return false;
    }

    TSet<FString> UniqueRecent;
    for (int32 i = 0; i < FMath::Min(4, RecentNpc.Num()); ++i)
    {
        UniqueRecent.Add(LocalTalkerNormalizeForLoopCompare(RecentNpc[i]->Content));
    }
    if (UniqueRecent.Num() <= 2)
    {
        return true;
    }

    int32 SimilarAdjacentPairs = 0;
    int32 SimilarSameSpeakerPairs = 0;
    for (int32 i = 0; i + 1 < RecentNpc.Num(); ++i)
    {
        if (LocalTalkerLikelySameUtterance(RecentNpc[i]->Content, RecentNpc[i + 1]->Content))
        {
            SimilarAdjacentPairs++;
        }
    }

    for (int32 i = 0; i < RecentNpc.Num(); ++i)
    {
        for (int32 j = i + 1; j < RecentNpc.Num(); ++j)
        {
            if (!RecentNpc[i]->SpeakerName.Equals(RecentNpc[j]->SpeakerName, ESearchCase::IgnoreCase))
            {
                continue;
            }
            if (LocalTalkerLikelySameUtterance(RecentNpc[i]->Content, RecentNpc[j]->Content))
            {
                SimilarSameSpeakerPairs++;
                break;
            }
        }
    }

    return (SimilarAdjacentPairs >= 1 && SimilarSameSpeakerPairs >= 1) || (SimilarSameSpeakerPairs >= 2);
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
    (void)DeltaTime;

    // Cleanup invalid actors from registry
    Registry.Remove(nullptr);

    const UWorld* W = GetWorld();
    const double Now = W ? (double)W->GetTimeSeconds() : 0.0;
    if (bPlayerSpeechPriorityActive && Now > PlayerSpeechPriorityUntilWorldSeconds)
    {
        UE_LOG(LogLocalTalker, Log, TEXT("%s[Director] PlayerSpeechPriority expired."), *LocalTalkerTimePrefixSubsystem(this));
        bPlayerSpeechPriorityActive = false;
        PlayerSpeechPriorityRadius = 0.0f;
        PlayerSpeechPriorityUntilWorldSeconds = 0.0;
    }
    if (bPlayerSpeechFenceActive && Now > PlayerSpeechFenceUntilWorldSeconds)
    {
        UE_LOG(LogLocalTalker, Log, TEXT("%s[Director] PlayerSpeechFence timed out."), *LocalTalkerTimePrefixSubsystem(this));
        bPlayerSpeechFenceActive = false;
        PlayerSpeechFenceRadius = 0.0f;
        PlayerSpeechFenceUntilWorldSeconds = 0.0;
    }
    
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

void ULocalTalkConversationSubsystem::RequestTurn(ULocalCharacterComponent* Talker, const FString& Prompt, bool bFromUser)
{
    if (!Talker || Prompt.IsEmpty()) return;

    const UWorld* W = GetWorld();
    const double Now = W ? (double)W->GetTimeSeconds() : 0.0;

    // User prompt path: record in history and keep a short priority window.
    if (bFromUser)
    {
        if (FLocalConversationContext* Context = FindOrCreateContext(Talker))
        {
            AddMessageToContext(*Context, TEXT("User"), Prompt, true);
        }

        if (Talker->GetOwner())
        {
            constexpr float UserPromptPrioritySeconds = 3.0f;
            SetPlayerSpeechPriorityWindow(
                Talker->GetOwner()->GetActorLocation(),
                FMath::Max(1.0f, Talker->GetHearingRadius()),
                UserPromptPrioritySeconds);
        }
    }

    EnqueueTurn(Talker, Prompt, Now, bFromUser);
}

void ULocalTalkConversationSubsystem::EnqueueTurn(ULocalCharacterComponent* Talker, const FString& Prompt, double EarliestGrantWorldSeconds, bool bFromUser)
{
    if (!Talker || Prompt.IsEmpty()) return;

    // Check if they are already in the queue - if so, update/upgrade in place.
    for (FQueuedTurn& Q : ManualQueue)
    {
        if (Q.Talker.Get() == Talker)
        {
            // Never downgrade an existing user request into a non-user one.
            if (Q.bFromUser && !bFromUser)
            {
                return;
            }

            Q.Prompt = Prompt;
            Q.bFromUser = bFromUser;

            // User prompts should be granted as soon as possible.
            if (bFromUser)
            {
                Q.EarliestGrantWorldSeconds = EarliestGrantWorldSeconds;
            }
            else
            {
                // Preserve the later of the existing earliest time and the new one.
                Q.EarliestGrantWorldSeconds = FMath::Max(Q.EarliestGrantWorldSeconds, EarliestGrantWorldSeconds);
            }
            return;
        }
    }

    FQueuedTurn NewTurn;
    NewTurn.Talker = Talker;
    NewTurn.Prompt = Prompt;
    NewTurn.bFromUser = bFromUser;
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

bool ULocalTalkConversationSubsystem::IsPlayerSpeechPriorityActive(double NowWorldSeconds) const
{
    return bPlayerSpeechPriorityActive && (NowWorldSeconds <= PlayerSpeechPriorityUntilWorldSeconds);
}

bool ULocalTalkConversationSubsystem::IsPlayerSpeechFenceActive(double NowWorldSeconds) const
{
    return bPlayerSpeechFenceActive && (NowWorldSeconds <= PlayerSpeechFenceUntilWorldSeconds);
}

bool ULocalTalkConversationSubsystem::IsTalkerWithinPlayerSpeechFence(const ULocalCharacterComponent* Talker, double NowWorldSeconds) const
{
    if (!IsPlayerSpeechFenceActive(NowWorldSeconds) || !Talker || !Talker->GetOwner())
    {
        return false;
    }

    const float Radius = FMath::Max(1.0f, PlayerSpeechFenceRadius);
    return FVector::DistSquared(Talker->GetOwner()->GetActorLocation(), PlayerSpeechFenceCenter) <= (Radius * Radius);
}

bool ULocalTalkConversationSubsystem::IsContextWithinPlayerSpeechFence(const FLocalConversationContext& Context, double NowWorldSeconds) const
{
    if (!IsPlayerSpeechFenceActive(NowWorldSeconds))
    {
        return false;
    }

    for (const TWeakObjectPtr<ULocalCharacterComponent>& Weak : Context.Participants)
    {
        if (const ULocalCharacterComponent* P = Weak.Get())
        {
            if (IsTalkerWithinPlayerSpeechFence(P, NowWorldSeconds))
            {
                return true;
            }
        }
    }

    const float Radius = FMath::Max(1.0f, PlayerSpeechFenceRadius);
    return FVector::DistSquared(Context.LastCenter, PlayerSpeechFenceCenter) <= (Radius * Radius);
}

bool ULocalTalkConversationSubsystem::IsTalkerWithinPlayerPriorityWindow(const ULocalCharacterComponent* Talker, double NowWorldSeconds) const
{
    if (IsTalkerWithinPlayerSpeechFence(Talker, NowWorldSeconds))
    {
        return true;
    }

    if (!IsPlayerSpeechPriorityActive(NowWorldSeconds) || !Talker || !Talker->GetOwner())
    {
        return false;
    }

    const float Radius = FMath::Max(1.0f, PlayerSpeechPriorityRadius);
    return FVector::DistSquared(Talker->GetOwner()->GetActorLocation(), PlayerSpeechPriorityCenter) <= (Radius * Radius);
}

bool ULocalTalkConversationSubsystem::IsContextWithinPlayerPriorityWindow(const FLocalConversationContext& Context, double NowWorldSeconds) const
{
    if (IsContextWithinPlayerSpeechFence(Context, NowWorldSeconds))
    {
        return true;
    }

    if (!IsPlayerSpeechPriorityActive(NowWorldSeconds))
    {
        return false;
    }

    for (const TWeakObjectPtr<ULocalCharacterComponent>& Weak : Context.Participants)
    {
        if (const ULocalCharacterComponent* P = Weak.Get())
        {
            if (IsTalkerWithinPlayerPriorityWindow(P, NowWorldSeconds))
            {
                return true;
            }
        }
    }

    const float Radius = FMath::Max(1.0f, PlayerSpeechPriorityRadius);
    return FVector::DistSquared(Context.LastCenter, PlayerSpeechPriorityCenter) <= (Radius * Radius);
}

void ULocalTalkConversationSubsystem::SetPlayerSpeechPriorityWindow(const FVector& Location, float Radius, float HoldSeconds)
{
    const UWorld* W = GetWorld();
    const double Now = W ? (double)W->GetTimeSeconds() : 0.0;
    const double Hold = (double)FMath::Max(0.0f, HoldSeconds);
    if (Hold <= 0.0)
    {
        return;
    }

    bPlayerSpeechPriorityActive = true;
    PlayerSpeechPriorityCenter = Location;
    PlayerSpeechPriorityRadius = FMath::Max(1.0f, Radius);
    PlayerSpeechPriorityUntilWorldSeconds = FMath::Max(PlayerSpeechPriorityUntilWorldSeconds, Now + Hold);

    UE_LOG(LogLocalTalker, Log,
        TEXT("%s[Director] PlayerSpeechPriority set center=(%.1f,%.1f,%.1f) radius=%.1f hold=%.2fs until=%.2f"),
        *LocalTalkerTimePrefixSubsystem(this),
        PlayerSpeechPriorityCenter.X,
        PlayerSpeechPriorityCenter.Y,
        PlayerSpeechPriorityCenter.Z,
        PlayerSpeechPriorityRadius,
        HoldSeconds,
        PlayerSpeechPriorityUntilWorldSeconds);
}

void ULocalTalkConversationSubsystem::BeginPlayerSpeechFence(const FVector& Location, float Radius, float MaxHoldSeconds)
{
    const UWorld* W = GetWorld();
    const double Now = W ? (double)W->GetTimeSeconds() : 0.0;
    const double Hold = (double)FMath::Max(0.0f, MaxHoldSeconds);
    if (Hold <= 0.0)
    {
        return;
    }

    bPlayerSpeechFenceActive = true;
    PlayerSpeechFenceCenter = Location;
    PlayerSpeechFenceRadius = FMath::Max(1.0f, Radius);
    PlayerSpeechFenceUntilWorldSeconds = FMath::Max(PlayerSpeechFenceUntilWorldSeconds, Now + Hold);

    UE_LOG(LogLocalTalker, Log,
        TEXT("%s[Director] PlayerSpeechFence set center=(%.1f,%.1f,%.1f) radius=%.1f hold=%.2fs until=%.2f"),
        *LocalTalkerTimePrefixSubsystem(this),
        PlayerSpeechFenceCenter.X,
        PlayerSpeechFenceCenter.Y,
        PlayerSpeechFenceCenter.Z,
        PlayerSpeechFenceRadius,
        MaxHoldSeconds,
        PlayerSpeechFenceUntilWorldSeconds);
}

void ULocalTalkConversationSubsystem::EndPlayerSpeechFence()
{
    if (!bPlayerSpeechFenceActive)
    {
        return;
    }

    bPlayerSpeechFenceActive = false;
    PlayerSpeechFenceRadius = 0.0f;
    PlayerSpeechFenceUntilWorldSeconds = 0.0;

    UE_LOG(LogLocalTalker, Log, TEXT("%s[Director] PlayerSpeechFence cleared."), *LocalTalkerTimePrefixSubsystem(this));
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
    const int32 MaxNpcTurns = (S->MaxConsecutiveNpcTurns > 0) ? S->MaxConsecutiveNpcTurns : 6;
    const bool bRequireListener = S->bRequirePlayerListenerForAuto;
    const bool bIgnoreListener = S->bKeepAliveIgnoresPlayerListenerRequirement;

    if (!bAllowNpcToNpc) return;

    const float Now = W->GetTimeSeconds();

    for (FLocalConversationContext& Context : ActiveContexts)
    {
        if (Context.Participants.Num() == 0) continue;
        if (IsContextWithinPlayerPriorityWindow(Context, (double)Now)) continue;
        if (Context.ConsecutiveNpcTurns >= MaxNpcTurns) continue;

        if (LocalTalkerContextLooksRepetitive(Context))
        {
            if ((Now - Context.LastLoopGuardLogTime) > 3.0f)
            {
                Context.LastLoopGuardLogTime = Now;
                UE_LOG(LogLocalTalker, Log,
                    TEXT("%s[Director] KeepAlive paused for repetitive NPC loop (npcTurns=%d cap=%d)."),
                    *LocalTalkerTimePrefixSubsystem(this),
                    Context.ConsecutiveNpcTurns,
                    MaxNpcTurns);
            }
            continue;
        }

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
            FString LastLine = LocalTalkerOneLineSubsystem(LastMsg.Content);
            LastLine.ReplaceInline(TEXT("\""), TEXT("'"));
            if (LastLine.Len() > 120)
            {
                LastLine = LastLine.Left(120) + TEXT("...");
            }
            Prompt = FString::Printf(
                TEXT("Director instruction: Keep the conversation alive. Respond in-character to %s's latest line \"%s\". In your first sentence, directly acknowledge that line. Add one NEW concrete detail, avoid repeating recent wording/themes, and ask at most one short follow-up question only if it helps move the conversation forward."),
                *LastMsg.SpeakerName,
                *LastLine
            );
        }
        else
        {
            Prompt = TEXT("Director instruction: Start a natural in-character conversation with the nearby person. Say something specific. You may ask one short question only if it clearly opens a new thread.");
        }

        // Enforce pacing via the queued turn's earliest-grant time.
        const double Earliest = (double)Now + (double)MinDelay;
        EnqueueTurn(Candidate, Prompt, Earliest, /*bFromUser*/false);
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
    const int32 MaxHistoryMessages = 24;
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
    const UWorld* W = GetWorld();
    const double NowSeconds = W ? (double)W->GetTimeSeconds() : 0.0;

    if (IsContextWithinPlayerPriorityWindow(Context, NowSeconds))
    {
        return;
    }

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
    const int32 MaxNpcTurns = S ? ((S->MaxConsecutiveNpcTurns > 0) ? S->MaxConsecutiveNpcTurns : 6) : 6;
    const float MinDelay = S ? S->MinSecondsBetweenAutoReplies : 0.0f;
    const float PostPauseMax = FMath::Max(0.0f, S ? S->PostTurnPauseMaxSeconds : 5.0f);
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
        if (LocalTalkerContextLooksRepetitive(Context))
        {
            const float Now = W ? W->GetTimeSeconds() : 0.0f;
            if ((Now - Context.LastLoopGuardLogTime) > 3.0f)
            {
                Context.LastLoopGuardLogTime = Now;
                UE_LOG(LogLocalTalker, Log,
                    TEXT("%s[Director] Auto-response paused for repetitive NPC loop (npcTurns=%d cap=%d)."),
                    *LocalTalkerTimePrefixSubsystem(this),
                    Context.ConsecutiveNpcTurns,
                    MaxNpcTurns);
            }
            return;
        }
        if (bRequireListener && !(bKeepAlive && bIgnoreListener) && !LocalTalkerIsAnyPlayerPawnInHearingRange(GetWorld(), Context))
        {
            return;
        }
        if (Context.ConsecutiveNpcTurns >= MaxNpcTurns)
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
                FString LastLine = LocalTalkerOneLineSubsystem(LastMsg.Content);
                LastLine.ReplaceInline(TEXT("\""), TEXT("'"));
                if (LastLine.Len() > 120)
                {
                    LastLine = LastLine.Left(120) + TEXT("...");
                }
                const FString Prompt = FString::Printf(
                    TEXT("Director instruction: Respond in-character to %s's latest line \"%s\". In your first sentence, directly acknowledge that line. Add one NEW detail or viewpoint, avoid repeating recent wording/themes, and ask at most one short follow-up question only if it advances the exchange."),
                    *LastMsg.SpeakerName,
                    *LastLine
                );
                UE_LOG(LogLocalTalker, Log, TEXT("%s[Director] -> TRIGGERING RESPONSE from '%s'"),
                    *LocalTalkerTimePrefixSubsystem(this),
                    *Candidate->GetSpeakerNameResolved());
                // Queue as a normal turn; add random pause so the player has a chance to speak.
                const double Now = (double)GetWorld()->GetTimeSeconds();
                const double RandomPause = (double)FMath::FRandRange(0.0f, PostPauseMax);
                const double Earliest = Now + (double)FMath::Max(0.0f, MinDelay) + RandomPause;
                EnqueueTurn(Candidate, Prompt, Earliest, /*bFromUser*/false);
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
            if (Context.ConsecutiveNpcTurns < MaxNpcTurns)
            {
                const float TimeSinceLast = GetWorld()->GetTimeSeconds() - Context.LastInteractionTime;
                if (TimeSinceLast < 5.0f)
                {
                    FString LastLine = LocalTalkerOneLineSubsystem(LastMsg.Content);
                    LastLine.ReplaceInline(TEXT("\""), TEXT("'"));
                    if (LastLine.Len() > 120)
                    {
                        LastLine = LastLine.Left(120) + TEXT("...");
                    }
                    const FString Prompt = FString::Printf(
                        TEXT("Director instruction: Continue speaking to the nearby listener in-character. Build from \"%s\", add one NEW angle not used in your recent lines, and ask at most one short question only if it moves the conversation forward."),
                        *LastLine
                    );
                    UE_LOG(LogLocalTalker, Log, TEXT("%s[Director] -> TRIGGERING SOLO CONTINUATION from '%s'"),
                        *LocalTalkerTimePrefixSubsystem(this),
                        *LastSpeaker->GetSpeakerNameResolved());
                    const double Now = (double)GetWorld()->GetTimeSeconds();
                    const double RandomPause = (double)FMath::FRandRange(0.0f, PostPauseMax);
                    const double Earliest = Now + (double)FMath::Max(0.0f, MinDelay) + RandomPause;
                    EnqueueTurn(LastSpeaker, Prompt, Earliest, /*bFromUser*/false);
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
    const int32 MaxConcurrentNpcTurns = S ? FMath::Clamp(S->MaxConcurrentNpcTurns, 1, 8) : 2;

    auto CountActiveNpcTurns = [this]() -> int32
    {
        int32 Count = 0;
        for (const TWeakObjectPtr<ULocalCharacterComponent>& Weak : Registry)
        {
            const ULocalCharacterComponent* Talker = Weak.Get();
            if (Talker && Talker->IsBusy())
            {
                ++Count;
            }
        }
        return Count;
    };

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

        if (!ManualQueue[i].bFromUser && IsTalkerWithinPlayerPriorityWindow(T, Now))
        {
            if (Now >= NextPlayerPriorityBlockedLogWorldSeconds)
            {
                NextPlayerPriorityBlockedLogWorldSeconds = Now + 1.0;
                UE_LOG(LogLocalTalker, Log,
                    TEXT("%s[Director] Deferring NPC turn for '%s' due to active player speech suppression."),
                    *LocalTalkerTimePrefixSubsystem(this),
                    *T->GetSpeakerNameResolved());
            }
            continue;
        }

        if (ManualQueue[i].bFromUser && T->IsBusy())
        {
            // User turns are highest priority: cut any in-flight generation/audio on the target now.
            T->Interrupt();
        }
        else if (T->IsBusy())
        {
            continue;
        }

        if (!ManualQueue[i].bFromUser && CountActiveNpcTurns() >= MaxConcurrentNpcTurns)
        {
            if (Now >= NextNpcConcurrencyBlockedLogWorldSeconds)
            {
                NextNpcConcurrencyBlockedLogWorldSeconds = Now + 1.0;
                UE_LOG(LogLocalTalker, Log,
                    TEXT("%s[Director] Deferring NPC turn for '%s' due to global NPC concurrency cap (%d)."),
                    *LocalTalkerTimePrefixSubsystem(this),
                    *T->GetSpeakerNameResolved(),
                    MaxConcurrentNpcTurns);
            }
            continue;
        }

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

        if (ManualQueue[i].bFromUser || !bContextBusy)
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
    int32 InterruptedCount = 0;
    for (const TWeakObjectPtr<ULocalCharacterComponent>& Weak : Registry)
    {
        ULocalCharacterComponent* T = Weak.Get();
        if (!T || !T->GetOwner()) continue;

        if (FVector::DistSquared(T->GetOwner()->GetActorLocation(), Location) <= RadiusSq)
        {
            T->Interrupt();
            InterruptedCount++;
        }
    }

    UE_LOG(LogLocalTalker, Log, TEXT("%s[Director] InterruptProximity radius=%.1f interrupted=%d"),
        *LocalTalkerTimePrefixSubsystem(this),
        Radius,
        InterruptedCount);
}

void ULocalTalkConversationSubsystem::CancelQueuedTurnsInProximity(const FVector& Location, float Radius)
{
    const float RadiusSq = Radius * Radius;
    const int32 Before = ManualQueue.Num();
    ManualQueue.RemoveAll([Location, RadiusSq](const FQueuedTurn& Q)
    {
        ULocalCharacterComponent* T = Q.Talker.Get();
        if (!T || !T->GetOwner())
        {
            return true;
        }
        if (Q.bFromUser)
        {
            return false;
        }
        return FVector::DistSquared(T->GetOwner()->GetActorLocation(), Location) <= RadiusSq;
    });

    const int32 Removed = Before - ManualQueue.Num();
    if (Removed > 0)
    {
        UE_LOG(LogLocalTalker, Log, TEXT("%s[Director] CancelQueuedTurnsInProximity radius=%.1f removedNpc=%d remaining=%d"),
            *LocalTalkerTimePrefixSubsystem(this),
            Radius,
            Removed,
            ManualQueue.Num());
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

#if WITH_EDITOR
int32 ULocalTalkConversationSubsystem::Test_GetManualQueueUserCount() const
{
    int32 Count = 0;
    for (const FQueuedTurn& Q : ManualQueue)
    {
        if (Q.bFromUser)
        {
            Count++;
        }
    }
    return Count;
}

int32 ULocalTalkConversationSubsystem::Test_GetManualQueueNpcCount() const
{
    int32 Count = 0;
    for (const FQueuedTurn& Q : ManualQueue)
    {
        if (!Q.bFromUser)
        {
            Count++;
        }
    }
    return Count;
}
#endif
