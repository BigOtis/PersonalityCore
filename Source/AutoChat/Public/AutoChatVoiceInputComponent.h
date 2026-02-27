#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "LocalTalkerTypes.h"
#include "HAL/ThreadSafeBool.h"
#include "AudioCaptureCore.h"
#include "AutoChatVoiceInputComponent.generated.h"

class ULocalCharacterComponent;
class UUserWidget;
class APlayerController;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FAutoChatVoiceSubmittedEvent, const FString&, Transcript, int32, NumTargets);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FAutoChatVoiceErrorEvent, const FString&, Error);

/**
 * Game-side player voice interaction component.
 * Keeps player interaction logic outside the LocalTalker plugin.
 */
UCLASS(ClassGroup=(AutoChat), meta=(BlueprintSpawnableComponent, DisplayName="AutoChat Voice Input"))
class AUTOCHAT_API UAutoChatVoiceInputComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UAutoChatVoiceInputComponent();

    /** Max candidate distance from player for nearby AI lookups. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice")
    float MaxInteractionRange = 1800.0f;

    /** If true, route to all nearby AIs. If false, route to nearest only. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice")
    bool bBroadcastToAllNearby = false;

    /** Apply selected mic settings onto target AI components before routing text. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice")
    bool bApplyMicSelectionToTargetAI = true;

    /** Selected mic mode for player input. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Microphone")
    ELocalTalkMicInputDeviceMode MicInputDeviceMode = ELocalTalkMicInputDeviceMode::DefaultSystem;

    /** Selected mic name (used when mode is NamedDevice). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Microphone")
    FString MicInputDeviceName;

    /** Optional widget class to display a simple microphone selector UI. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|UI")
    TSubclassOf<UUserWidget> MicSelectorWidgetClass;

    /** If true, show on-screen subtitles for the player's recognized speech. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Subtitles")
    bool bShowPlayerSubtitles = true;

    /** Display name used as the speaker label for player subtitles (e.g., "You"). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Subtitles")
    FString PlayerSubtitleSpeakerName;

    /** Heuristic seconds-per-character used to estimate how long to display player subtitles. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Subtitles", meta=(ClampMin="0.01"))
    float PlayerSubtitleSecondsPerChar = 0.06f;

    UPROPERTY(BlueprintAssignable, Category="AutoChat|Voice")
    FAutoChatVoiceSubmittedEvent OnTranscriptSubmitted;

    UPROPERTY(BlueprintAssignable, Category="AutoChat|Voice")
    FAutoChatVoiceErrorEvent OnVoiceError;

    /** Returns available microphone names from LocalTalker settings/device enumeration. */
    UFUNCTION(BlueprintCallable, Category="AutoChat|Microphone")
    TArray<FString> GetAvailableMicrophones() const;

    /** Sets selected microphone to system default. */
    UFUNCTION(BlueprintCallable, Category="AutoChat|Microphone")
    void UseDefaultMicrophone();

    /** Sets selected microphone to specific device name. */
    UFUNCTION(BlueprintCallable, Category="AutoChat|Microphone")
    void UseNamedMicrophone(const FString& DeviceName);

    /** Placeholder hook for runtime STT. Returns false unless STT backend is integrated. */
    UFUNCTION(BlueprintCallable, Category="AutoChat|Voice")
    bool StartVoiceCaptureAndTranscribe();

    /**
     * Submit transcribed text into nearby AI conversation.
     * This is the handoff point from STT output to LocalTalker interaction.
     */
    UFUNCTION(BlueprintCallable, Category="AutoChat|Voice")
    bool SubmitRecognizedSpeech(const FString& Transcript);

    /** Test/helper alias for SubmitRecognizedSpeech. */
    UFUNCTION(BlueprintCallable, Category="AutoChat|Voice")
    bool SubmitSimulatedVoiceInput(const FString& Transcript) { return SubmitRecognizedSpeech(Transcript); }

    /** Finds all AIs that can hear the player and are within max range. */
    UFUNCTION(BlueprintCallable, Category="AutoChat|Voice")
    TArray<ULocalCharacterComponent*> FindNearbyAIs(float OverrideRange = -1.0f) const;

    /** Spawns the mic selector widget and adds it to the viewport. */
    UFUNCTION(BlueprintCallable, Category="AutoChat|UI")
    bool ShowMicSelectorUI(APlayerController* OwningPlayer = nullptr);

    /** Removes the active mic selector widget from the viewport. */
    UFUNCTION(BlueprintCallable, Category="AutoChat|UI")
    void HideMicSelectorUI();

private:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    void ApplyMicSelectionToAI(ULocalCharacterComponent* AI) const;

    void ShowPlayerSubtitle(const FString& Transcript, int32 NumTargets);

    UPROPERTY(Transient)
    TObjectPtr<UUserWidget> ActiveMicSelectorWidget = nullptr;

    // Lightweight mic level debugger: opens an AudioCapture stream and pings
    // the mic widget when non-silent audio is observed. This is purely for
    // visualization / confirmation that the selected device is delivering audio.
    Audio::FAudioCapture MicCapture;
    FThreadSafeBool bMicCaptureActive = false;

    void StartMicCapture();
    void StopMicCapture();

    void OnAudioCapture(const float* AudioData, int32 NumFrames, int32 NumChannels, double StreamTime, bool bOverflow);
};
