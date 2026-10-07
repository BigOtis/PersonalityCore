#pragma once
#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "LocalTalkerSettings.generated.h"

UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="PersonalityCore"))
class LOCALTALKER_API ULocalTalkerSettings : public UDeveloperSettings {
 GENERATED_BODY()
public:
 UPROPERTY(Config, EditAnywhere, Category="Connection") FString RuntimeUrl=TEXT("http://127.0.0.1:8765");
 UPROPERTY(Config, EditAnywhere, Category="Microphone") int32 CaptureDeviceIndex=-1;
};
