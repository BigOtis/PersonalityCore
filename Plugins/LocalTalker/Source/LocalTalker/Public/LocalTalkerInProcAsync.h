#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "LocalTalkerTypes.h"
#include "LocalTalkerInProcAsync.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLocalTalkerInProcTokenEvent, const FString&, Token);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLocalTalkerInProcTextEvent, const FString&, Text);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLocalTalkerInProcErrorEvent, const FString&, Error);

UCLASS()
class LOCALTALKER_API ULocalTalkerInProcGenerateAsync : public UBlueprintAsyncActionBase
{
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintAssignable)
    FLocalTalkerInProcTokenEvent OnToken;

    UPROPERTY(BlueprintAssignable)
    FLocalTalkerInProcTextEvent OnDelta;

    UPROPERTY(BlueprintAssignable)
    FLocalTalkerInProcTextEvent OnCompleted;

    UPROPERTY(BlueprintAssignable)
    FLocalTalkerInProcErrorEvent OnError;

    UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"), Category="LocalTalker")
    static ULocalTalkerInProcGenerateAsync* GenerateStreamingInProc(
        UObject* WorldContextObject,
        const FLocalTalkerRuntimePaths& Paths,
        const FLocalTalkerCharacterConfig& Character,
        const FString& UserPrompt
    );

    // Advanced: provide a fully constructed prompt text (includes system/directions + conversation + "Assistant:" suffix, etc).
    UFUNCTION(BlueprintCallable, meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"), Category="LocalTalker")
    static ULocalTalkerInProcGenerateAsync* GenerateStreamingInProcWithPromptText(
        UObject* WorldContextObject,
        const FLocalTalkerRuntimePaths& Paths,
        const FLocalTalkerCharacterConfig& Character,
        const FString& PromptText
    );

    UFUNCTION(BlueprintCallable, Category="LocalTalker")
    void Cancel();

    virtual void Activate() override;

private:
    UPROPERTY()
    UObject* WorldContextObject = nullptr;

    FLocalTalkerRuntimePaths Paths;
    FLocalTalkerCharacterConfig Character;
    FString UserPrompt;
    FString PromptText;
    bool bPromptIsFull = false;

    FThreadSafeBool bCancel = false;

    void DispatchError(const FString& Msg);
};
