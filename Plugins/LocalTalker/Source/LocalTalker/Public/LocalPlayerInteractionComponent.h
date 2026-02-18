#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "LocalPlayerInteractionComponent.generated.h"

class ULocalCharacterComponent;

/**
 * Player-side helper for interacting with nearby LocalTalk AI characters.
 * This component is intentionally separate from ULocalCharacterComponent.
 */
UCLASS(ClassGroup=(LocalTalk), meta=(BlueprintSpawnableComponent, DisplayName="LocalTalk Player Interaction"))
class LOCALTALKER_API ULocalPlayerInteractionComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    ULocalPlayerInteractionComponent();

    /** Default max range for nearest-AI lookups. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    float InteractionRange = 1500.0f;

    /** Finds the nearest registered AI talker within range. */
    UFUNCTION(BlueprintCallable, Category="LocalTalker")
    ULocalCharacterComponent* FindNearestAI(float MaxRange = -1.0f) const;

    /** Sends player text to a specific AI; returns false if target/text is invalid. */
    UFUNCTION(BlueprintCallable, Category="LocalTalker")
    bool SpeakToAI(ULocalCharacterComponent* TargetAI, const FString& PlayerText) const;

    /** Convenience: find nearest AI and send player text. */
    UFUNCTION(BlueprintCallable, Category="LocalTalker")
    bool SpeakToNearestAI(const FString& PlayerText, float MaxRange = -1.0f) const;
};

