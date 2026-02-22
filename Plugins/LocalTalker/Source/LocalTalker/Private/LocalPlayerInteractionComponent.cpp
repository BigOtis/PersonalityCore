#include "LocalPlayerInteractionComponent.h"
#include "LocalCharacterComponent.h"
#include "LocalTalkConversationSubsystem.h"

#include "Engine/World.h"
#include "UObject/UObjectIterator.h"

ULocalPlayerInteractionComponent::ULocalPlayerInteractionComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

ULocalCharacterComponent* ULocalPlayerInteractionComponent::FindNearestAI(float MaxRange) const
{
    const AActor* Owner = GetOwner();
    UWorld* World = GetWorld();
    if (!World && Owner)
    {
        World = Owner->GetWorld();
    }
    if (!Owner || !World)
    {
        return nullptr;
    }

    const float EffectiveRange = (MaxRange > 0.0f) ? MaxRange : InteractionRange;
    const float EffectiveRangeSq = (EffectiveRange > 0.0f) ? (EffectiveRange * EffectiveRange) : TNumericLimits<float>::Max();
    const FVector OwnerLoc = Owner->GetActorLocation();

    ULocalCharacterComponent* Best = nullptr;
    float BestDistSq = TNumericLimits<float>::Max();
    ULocalCharacterComponent* BestAny = nullptr;
    float BestAnyDistSq = TNumericLimits<float>::Max();
    auto ConsiderTalker = [&](ULocalCharacterComponent* Talker)
    {
        if (!Talker || !Talker->GetOwner())
        {
            return;
        }
        if (Talker->GetOwner() == Owner)
        {
            return;
        }

        const float DistSq = FVector::DistSquared(OwnerLoc, Talker->GetOwner()->GetActorLocation());
        if (DistSq < BestAnyDistSq)
        {
            BestAnyDistSq = DistSq;
            BestAny = Talker;
        }

        if (DistSq > EffectiveRangeSq)
        {
            return;
        }

        if (DistSq < BestDistSq)
        {
            BestDistSq = DistSq;
            Best = Talker;
        }
    };

    if (ULocalTalkConversationSubsystem* Sub = World->GetSubsystem<ULocalTalkConversationSubsystem>())
    {
        for (ULocalCharacterComponent* Talker : Sub->GetRegisteredTalkers())
        {
            ConsiderTalker(Talker);
        }
    }

    // Fallback for cases where the subsystem registry is not initialized/populated yet.
    if (!Best)
    {
        for (TObjectIterator<ULocalCharacterComponent> It; It; ++It)
        {
            ULocalCharacterComponent* Talker = *It;
            if (!Talker || Talker->HasAnyFlags(RF_ClassDefaultObject))
            {
                continue;
            }
            if (Talker->GetWorld() != World)
            {
                continue;
            }
            ConsiderTalker(Talker);
        }
    }

    // Rootless owner actors in editor automation can report V(0), which makes strict range checks unusable.
    // In that case, fall back to the globally nearest candidate.
    if (!Best && !Owner->GetRootComponent() && BestAny)
    {
        return BestAny;
    }

    return Best;
}

bool ULocalPlayerInteractionComponent::SpeakToAI(ULocalCharacterComponent* TargetAI, const FString& PlayerText) const
{
    if (!TargetAI || PlayerText.IsEmpty())
    {
        return false;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        const AActor* Owner = GetOwner();
        if (Owner)
        {
            World = Owner->GetWorld();
        }
    }
    if (!World && TargetAI)
    {
        World = TargetAI->GetWorld();
    }
    if (!World)
    {
        return false;
    }

    ULocalTalkConversationSubsystem* Sub = World->GetSubsystem<ULocalTalkConversationSubsystem>();
    if (!Sub)
    {
        return false;
    }

    Sub->RequestTurn(TargetAI, PlayerText);
    return true;
}

bool ULocalPlayerInteractionComponent::SpeakToNearestAI(const FString& PlayerText, float MaxRange) const
{
    ULocalCharacterComponent* Target = FindNearestAI(MaxRange);
    return SpeakToAI(Target, PlayerText);
}
