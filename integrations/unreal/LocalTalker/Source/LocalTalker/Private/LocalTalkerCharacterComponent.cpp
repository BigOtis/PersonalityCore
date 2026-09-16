#include "LocalTalkerCharacterComponent.h"
#include "LocalTalkerSubsystem.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundWaveProcedural.h"
#include "Engine/GameInstance.h"
#include "GameFramework/Actor.h"

ULocalTalkerCharacterComponent::ULocalTalkerCharacterComponent() { PrimaryComponentTick.bCanEverTick=false; }
void ULocalTalkerCharacterComponent::BeginPlay() {
 Super::BeginPlay();
 Audio=NewObject<UAudioComponent>(GetOwner());
 Audio->bAutoActivate=false;
 Audio->SetupAttachment(GetOwner()->GetRootComponent());
 Audio->bAllowSpatialization=bSpatializeVoice;
 Audio->bOverrideAttenuation=true;
 Audio->AttenuationOverrides.bAttenuate=bSpatializeVoice;
 Audio->AttenuationOverrides.bSpatialize=bSpatializeVoice;
 Audio->AttenuationOverrides.AttenuationShapeExtents=FVector(250);
 Audio->AttenuationOverrides.FalloffDistance=2400;
 Audio->RegisterComponent();
 if (auto* GI=GetWorld()->GetGameInstance()) GI->GetSubsystem<ULocalTalkerSubsystem>()->RegisterCharacter(this);
}
void ULocalTalkerCharacterComponent::QueueVoice(const TArray<uint8>& PCM, int32 SampleRate) {
 if (!Audio || PCM.IsEmpty()) return;
 if (!Wave) {
  Wave=NewObject<USoundWaveProcedural>(this);
  Wave->SetSampleRate(SampleRate); Wave->NumChannels=1;
  Wave->Duration=INDEFINITELY_LOOPING_DURATION;
  Wave->SoundGroup=SOUNDGROUP_Voice;
  Audio->SetSound(Wave);
 }
 Wave->QueueAudio(PCM.GetData(),PCM.Num());
 if (!Audio->IsPlaying()) Audio->Play();
 bSpeaking=true;
}
void ULocalTalkerCharacterComponent::StopVoice() {
 if (Audio) Audio->Stop();
 if (Wave) Wave->ResetAudio();
 Wave=nullptr; bSpeaking=false;
}
void ULocalTalkerCharacterComponent::EndPlay(const EEndPlayReason::Type Reason) {
 StopVoice();
 if (auto* GI=GetWorld()->GetGameInstance()) GI->GetSubsystem<ULocalTalkerSubsystem>()->UnregisterCharacter(this);
 Super::EndPlay(Reason);
}
