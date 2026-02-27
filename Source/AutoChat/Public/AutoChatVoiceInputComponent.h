#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "LocalTalkerTypes.h"
#include "HAL/ThreadSafeBool.h"
#include "AudioCaptureCore.h"
#include "AutoChatVoiceInputComponent.generated.h"

class ULocalCharacterComponent;
class ULocalPlayerInteractionComponent;
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

    /** If true, interrupt nearby AI speech when player begins voice capture (barge-in behavior). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice")
    bool bInterruptNearbyAIOnVoiceCaptureStart = true;

    /** Minimum RMS to trigger barge-in (interrupt AI). Use a higher value than AutoTranscribeStartRmsThreshold to avoid noise. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.001", ClampMax="1.0"))
    float BargeInMinRmsThreshold = 0.035f;

    /** If true, clear queued NPC turns near the player when voice capture begins so new player speech is prioritized. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice")
    bool bCancelQueuedTurnsOnVoiceCaptureStart = true;

    /** Default mode: continuously listen and auto-transcribe completed speech segments (no push-to-talk required). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice")
    bool bAlwaysOnAutoTranscribe = true;

    /** RMS threshold to start a speech segment in always-on mode. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.001", ClampMax="1.0"))
    float AutoTranscribeStartRmsThreshold = 0.020f;

    /** RMS threshold to continue speech segment in always-on mode. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.001", ClampMax="1.0"))
    float AutoTranscribeContinueRmsThreshold = 0.012f;

    /** Silence time required to end speech segment and trigger transcription. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.10", ClampMax="3.0"))
    float AutoTranscribeSilenceSeconds = 0.65f;

    /** Minimum utterance length (seconds) required to submit transcription. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.10", ClampMax="5.0"))
    float AutoTranscribeMinSpeechSeconds = 0.35f;

    /** Hard cap for a single utterance in always-on mode. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="1.0", ClampMax="30.0"))
    float AutoTranscribeMaxSpeechSeconds = 12.0f;

    /** Seconds to keep Director-level player priority active after a loud barge-in interrupt. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.25", ClampMax="20.0"))
    float BargeInPriorityHoldSeconds = 4.0f;

    /** Seconds to keep player priority active after an always-on segment is sent to Whisper. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.25", ClampMax="30.0"))
    float PostSegmentPriorityHoldSeconds = 8.0f;

    /** Seconds to keep player priority active when a transcript is submitted to nearby AI. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.25", ClampMax="20.0"))
    float PostTranscriptPriorityHoldSeconds = 4.0f;

    /** Hard fence max hold while player speech is being captured/transcribed (failsafe timeout). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="2.0", ClampMax="120.0"))
    float PlayerSpeechFenceMaxSeconds = 30.0f;

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

    /** Minimum transcript length accepted for routing (after normalization). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="1", ClampMax="200"))
    int32 MinAcceptedTranscriptChars = 6;

    /** Hard cap for transcript length accepted for routing (longer text is truncated). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="32", ClampMax="2000"))
    int32 MaxAcceptedTranscriptChars = 280;

    /** Repetition guard: when transcript has many sentences, reject if one repeated sentence dominates. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="3", ClampMax="64"))
    int32 RepetitionSentenceThreshold = 6;

    /** Repetition guard dominant ratio (0-1). Higher means less strict filtering. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.50", ClampMax="1.0"))
    float MaxDominantSentenceRatio = 0.72f;

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

    /** Backward-compatible toggle: first call begins capture, next call ends capture and transcribes. */
    UFUNCTION(BlueprintCallable, Category="AutoChat|Voice")
    bool StartVoiceCaptureAndTranscribe();

    /** Explicitly begin player voice capture (push-to-talk pressed). */
    UFUNCTION(BlueprintCallable, Category="AutoChat|Voice")
    bool BeginVoiceCapture();

    /** Explicitly end player voice capture and request Whisper transcription (push-to-talk released). */
    UFUNCTION(BlueprintCallable, Category="AutoChat|Voice")
    bool EndVoiceCaptureAndTranscribe();

    /** Cancel active player voice capture without transcription. */
    UFUNCTION(BlueprintCallable, Category="AutoChat|Voice")
    void CancelVoiceCapture();

    /** Returns true while player voice capture is active. */
    UFUNCTION(BlueprintCallable, Category="AutoChat|Voice")
    bool IsVoiceCaptureActive() const { return bWhisperCaptureInProgress; }

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
    void InterruptNearbyAIsForPlayerSpeech();
    void ApplyPlayerSpeechPriorityWindow(float HoldSeconds, const TCHAR* Reason);
    void BeginPlayerSpeechFence(float MaxHoldSeconds, const TCHAR* Reason);
    void EndPlayerSpeechFence(const TCHAR* Reason);
    FString NormalizeTranscriptForRouting(const FString& InText) const;
    bool IsTranscriptLikelyRepetitionSpam(const FString& InText, FString& OutReason) const;

    UPROPERTY(Transient)
    TObjectPtr<UUserWidget> ActiveMicSelectorWidget = nullptr;

    // Lightweight mic level debugger: opens an AudioCapture stream and pings
    // the mic widget when non-silent audio is observed. This is purely for
    // visualization / confirmation that the selected device is delivering audio.
    Audio::FAudioCapture MicCapture;
    FThreadSafeBool bMicCaptureActive = false;
    double NextMicActivityLogTimeSeconds = 0.0;

    void StartMicCapture();
    void StopMicCapture();

    void OnAudioCapture(const float* AudioData, int32 NumFrames, int32 NumChannels, int32 SampleRate, double StreamTime, bool bOverflow);
    void ProcessAlwaysOnAutoTranscribe(const float* AudioData, int32 NumFrames, int32 NumChannels, int32 SampleRate, float Rms);

    bool EnsureWhisperBridge();

    UFUNCTION()
    void HandleWhisperTranscription(const FString& Transcript);

    UFUNCTION()
    void HandleWhisperError(const FString& Error);

    UPROPERTY(Transient)
    TObjectPtr<ULocalPlayerInteractionComponent> WhisperBridgeComponent = nullptr;

    bool bWhisperCaptureInProgress = false;
    bool bAutoSpeechSegmentActive = false;
    bool bAutoSpeechSegmentBargeInTriggered = false;
    bool bAutoTranscribeRequestInFlight = false;
    double AutoSpeechStartWorldSeconds = 0.0;
    double AutoSpeechLastActiveWorldSeconds = 0.0;
    int32 AutoSpeechSampleRate = 0;
    int32 AutoSpeechNumChannels = 0;
    TArray<int16> AutoSpeechPcm16;

    /** World time when we last submitted a transcript; used to skip barge-in briefly so the AI can respond. */
    double LastTranscriptSubmitWorldSeconds = 0.0;
    /** Seconds after submitting a transcript during which segment START does not trigger barge-in. */
    static constexpr double TranscriptSubmitBargeInGraceSeconds = 2.5;
};
