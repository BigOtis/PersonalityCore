#pragma once
#include "CoreMinimal.h"

struct FLocalWavPcm16
{
    int32 SampleRate = 0;
    int32 NumChannels = 0;
    TArray<int16> Samples;
};

class FLocalTalkerWav
{
public:
    static bool LoadWavPcm16(const FString& Path, FLocalWavPcm16& Out, FString& OutError);
    static bool SaveWavPcm16(const FString& Path, const FLocalWavPcm16& In, FString& OutError);
};
