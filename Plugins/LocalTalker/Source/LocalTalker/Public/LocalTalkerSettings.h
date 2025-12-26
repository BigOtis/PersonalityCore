#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "LocalTalkerTypes.h"
#include "LocalTalkerSettings.generated.h"

UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="LocalTalker"))
class LOCALTALKER_API ULocalTalkerSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    ULocalTalkerSettings();

    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Paths")
    FLocalTalkerRuntimePaths DefaultPaths;

    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Defaults")
    FLocalTalkerCharacterConfig DefaultCharacterConfig;

    // Bundled/local voice options (used to populate component dropdown).
    UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category="Voices")
    TArray<FLocalTalkVoiceOption> Voices;
};
