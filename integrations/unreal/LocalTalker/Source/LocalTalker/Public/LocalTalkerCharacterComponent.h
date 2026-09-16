#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "LocalTalkerCharacterComponent.generated.h"
class USoundWaveProcedural;
class UAudioComponent;

/** Attach to any actor. CharacterKey matches a member in the engine-neutral scene JSON. */
UCLASS(ClassGroup=(LocalTalker), meta=(BlueprintSpawnableComponent))
class LOCALTALKER_API ULocalTalkerCharacterComponent : public UActorComponent {
 GENERATED_BODY()
public:
 ULocalTalkerCharacterComponent();
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker") FString CharacterKey;
 UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker") bool bSpatializeVoice=true;
 UPROPERTY(BlueprintReadOnly, Category="LocalTalker") bool bSpeaking=false;
 UFUNCTION(BlueprintCallable, Category="LocalTalker") void StopVoice();
 void QueueVoice(const TArray<uint8>& PCM, int32 SampleRate);
 virtual void BeginPlay() override;
 virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
private:
 UPROPERTY(Transient) TObjectPtr<USoundWaveProcedural> Wave;
 UPROPERTY(Transient) TObjectPtr<UAudioComponent> Audio;
};
