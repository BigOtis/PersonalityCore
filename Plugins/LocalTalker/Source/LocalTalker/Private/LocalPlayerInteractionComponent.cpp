#include "LocalPlayerInteractionComponent.h"
#include "LocalCharacterComponent.h"
#include "LocalTalkConversationSubsystem.h"

#include "Engine/World.h"

ULocalPlayerInteractionComponent::ULocalPlayerInteractionComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

ULocalCharacterComponent* ULocalPlayerInteractionComponent::FindNearestAI(float MaxRange) const
{
    const AActor* Owner = GetOwner();
    UWorld* World = GetWorld();
    if (!Owner || !World)
    {
        return nullptr;
    }

    ULocalTalkConversationSubsystem* Sub = World->GetSubsystem<ULocalTalkConversationSubsystem>();
    if (!Sub)
    {
        return nullptr;
    }

    const float EffectiveRange = (MaxRange > 0.0f) ? MaxRange : InteractionRange;
    const float EffectiveRangeSq = (EffectiveRange > 0.0f) ? (EffectiveRange * EffectiveRange) : TNumericLimits<float>::Max();
    const FVector OwnerLoc = Owner->GetActorLocation();

    ULocalCharacterComponent* Best = nullptr;
    float BestDistSq = TNumericLimits<float>::Max();

    for (ULocalCharacterComponent* Talker : Sub->GetRegisteredTalkers())
    {
        if (!Talker || !Talker->GetOwner())
        {
            continue;
        }
        if (Talker->GetOwner() == Owner)
        {
            continue;
        }

        const float DistSq = FVector::DistSquared(OwnerLoc, Talker->GetOwner()->GetActorLocation());
        if (DistSq > EffectiveRangeSq)
        {
            continue;
        }

        if (DistSq < BestDistSq)
        {
            BestDistSq = DistSq;
            Best = Talker;
        }
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

