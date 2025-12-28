#include "LocalTalkConversationSubsystem.h"
#include "LocalCharacterComponent.h"
#include "LocalTalkerSettings.h"
#include "LocalTalkerLog.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"

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

        // "In range to hear" = within any participant's ConversationRadius.
        for (const auto& WeakP : Context.Participants)
        {
            const ULocalCharacterComponent* P = WeakP.Get();
            if (!P || !P->GetOwner()) continue;
            const float R = FMath::Max(0.0f, P->ConversationRadius);
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
    UE_LOG(LogLocalTalker, Log, TEXT("[Director] Conversation Subsystem Initialized."));
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
}

void ULocalTalkConversationSubsystem::RegisterTalker(ULocalCharacterComponent* Talker)
{
    if (!Talker) return;
    Registry.Add(Talker);
    UE_LOG(LogLocalTalker, Log, TEXT("[Director] Registered: '%s' (Total: %d)"), 
        *Talker->GetSpeakerNameResolved(), Registry.Num());
}

void ULocalTalkConversationSubsystem::UnregisterTalker(ULocalCharacterComponent* Talker)
{
    if (!Talker) return;
    
    FString Name = Talker->GetSpeakerNameResolved();
    Registry.Remove(Talker);
    
    // Remove from manual queue if they were waiting
    ManualQueue.RemoveAll([Talker](const FQueuedTurn& T) { return T.Talker.Get() == Talker; });

    UE_LOG(LogLocalTalker, Log, TEXT("[Director] Unregistered: '%s'"), *Name);
}

void ULocalTalkConversationSubsystem::RequestTurn(ULocalCharacterComponent* Talker, const FString& Prompt)
{
    if (!Talker || Prompt.IsEmpty()) return;

    // If it's a user prompt (not RAW and not an instruction), add to history
    if (!Prompt.StartsWith(TEXT("RAW:")) && !Prompt.StartsWith(TEXT("Respond to ")))
    {
        if (FLocalConversationContext* Context = FindOrCreateContext(Talker))
        {
            AddMessageToContext(*Context, TEXT("User"), Prompt, true);
        }
    }

    // Check if they are already in the queue - if so, update their prompt
    for (FQueuedTurn& Q : ManualQueue)
    {
        if (Q.Talker.Get() == Talker)
        {
            Q.Prompt = Prompt;
            return;
        }
    }

    ManualQueue.Add({ Talker, Prompt });
    
    UE_LOG(LogLocalTalker, Log, TEXT("[Director] '%s' requested turn (Manual Queue length: %d)"), 
        *Talker->GetSpeakerNameResolved(), ManualQueue.Num());
}

void ULocalTalkConversationSubsystem::ReleaseTurn(ULocalCharacterComponent* Talker)
{
    if (!Talker) return;
    
    UE_LOG(LogLocalTalker, Log, TEXT("[Director] '%s' finished talking."), *Talker->GetSpeakerNameResolved());

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
    UE_LOG(LogLocalTalker, Log, TEXT("[Director] %s: %s"), *SpeakerName, *Text);

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

void ULocalTalkConversationSubsystem::UpdateContexts()
{
    UWorld* W = GetWorld();
    if (!W) return;
    float CurrentTime = W->GetTimeSeconds();

    // 1. Cleanup old contexts (inactive for > 30s)
    ActiveContexts.RemoveAll([CurrentTime](const FLocalConversationContext& C) {
        return (CurrentTime - C.LastInteractionTime) > 30.0f;
    });

    // 2. Refresh participants for each context based on proximity to context center
    for (auto& Context : ActiveContexts)
    {
        Context.Participants.Empty();
        for (auto& WeakAgent : Registry)
        {
            ULocalCharacterComponent* Agent = WeakAgent.Get();
            if (!Agent || !Agent->GetOwner()) continue;

            float Dist = FVector::Dist(Agent->GetOwner()->GetActorLocation(), Context.LastCenter);
            if (Dist < Agent->ConversationRadius)
            {
                Context.Participants.Add(Agent);
            }
        }
    }
}

FLocalConversationContext* ULocalTalkConversationSubsystem::FindOrCreateContext(ULocalCharacterComponent* Agent)
{
    if (!Agent || !Agent->GetOwner()) return nullptr;
    FVector Loc = Agent->GetOwner()->GetActorLocation();

    // Try to find an existing context near this location
    for (auto& Context : ActiveContexts)
    {
        if (FVector::Dist(Context.LastCenter, Loc) < Agent->ConversationRadius)
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
    return &ActiveContexts[Index];
}

void ULocalTalkConversationSubsystem::AddMessageToContext(FLocalConversationContext& Context, const FString& Speaker, const FString& Text, bool bFromUser)
{
    Context.History.Add({ Speaker, Text, bFromUser });
    if (Context.History.Num() > 10) Context.History.RemoveAt(0);
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
    // Don't start a new turn if someone is still busy (LLM processing or TTS speaking)
    for (auto& Weak : Context.Participants)
    {
        if (ULocalCharacterComponent* P = Weak.Get())
        {
            if (P->IsBusy()) return;
        }
    }

    if (Context.History.Num() <= 0)
    {
        return;
    }

    const FLocalTalkMessage& LastMsg = Context.History.Last();

    const ULocalTalkerSettings* S = GetDefault<ULocalTalkerSettings>();
    const bool bAllowNpcToNpc = S ? S->bAllowNpcToNpcAuto : false;
    const int32 MaxNpcTurns = S ? S->MaxConsecutiveNpcTurns : 0;
    const float MinDelay = S ? S->MinSecondsBetweenAutoReplies : 0.0f;
    const bool bRequireListener = S ? S->bRequirePlayerListenerForAuto : false;

    // Default rule: auto-respond to USER messages. Optionally allow NPC-to-NPC within a capped streak.
    if (!LastMsg.bFromUser)
    {
        if (!bAllowNpcToNpc)
        {
            return;
        }
        if (bRequireListener && !LocalTalkerIsAnyPlayerPawnInHearingRange(GetWorld(), Context))
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
    for (auto& Weak : Context.Participants)
    {
        ULocalCharacterComponent* Candidate = Weak.Get();
        if (Candidate && Candidate != LastSpeaker && !Candidate->IsBusy())
        {
            // Only respond if the last message was within a reasonable timeframe
            const float TimeSinceLast = GetWorld()->GetTimeSeconds() - Context.LastInteractionTime;
            if (TimeSinceLast >= MinDelay && TimeSinceLast < 5.0f)
            {
                FString Prompt = FString::Printf(TEXT("Respond to %s: %s"), *LastMsg.SpeakerName, *LastMsg.Content);
                UE_LOG(LogLocalTalker, Log, TEXT("[Director] -> TRIGGERING RESPONSE from '%s'"), *Candidate->GetSpeakerNameResolved());
                Candidate->InternalGrantTurn(Prompt);
                return;
            }
        }
    }
}

void ULocalTalkConversationSubsystem::ProcessTurns()
{
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

        if (T->IsBusy()) continue;

        // Check if ANYONE in this talker's context is busy
        bool bContextBusy = false;
        if (FLocalConversationContext* Context = FindOrCreateContext(T))
        {
            for (auto& Weak : Context->Participants)
            {
                if (ULocalCharacterComponent* P = Weak.Get())
                {
                    if (P->IsBusy())
                    {
                        bContextBusy = true;
                        break;
                    }
                }
            }
        }

        if (!bContextBusy)
        {
            FString Prompt = ManualQueue[i].Prompt;
            ManualQueue.RemoveAt(i);
            i--;

            UE_LOG(LogLocalTalker, Log, TEXT("[Director] -> GRANTING TURN to: '%s'"), *T->GetSpeakerNameResolved());
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
