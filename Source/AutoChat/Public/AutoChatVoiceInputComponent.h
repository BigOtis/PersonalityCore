#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "LocalTalkerTypes.h"
#include "HAL/ThreadSafeBool.h"
#include "AudioCaptureCore.h"
#include "AutoChatVoiceInputComponent.generated.h"

class ULocalCharacterComponent;
class ULocalPlayerInteractionComponent;
class ULocalTalkConversationSubsystem;
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

    /** If true and not broadcasting, rotate target selection across nearby AIs instead of always nearest. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice")
    bool bRotateSingleTargetAcrossNearby = false;

    /** Apply selected mic settings onto target AI components before routing text. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice")
    bool bApplyMicSelectionToTargetAI = true;

    /** If true, interrupt nearby AI speech when player begins voice capture (barge-in behavior). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice")
    bool bInterruptNearbyAIOnVoiceCaptureStart = true;

    /** Minimum RMS to trigger barge-in (interrupt AI). Use a higher value than AutoTranscribeStartRmsThreshold to avoid noise. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.001", ClampMax="1.0"))
    float BargeInMinRmsThreshold = 0.045f;

    /** Require this much sustained active speech before barge-in can interrupt AI. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.01", ClampMax="2.0"))
    float BargeInMinActiveSpeechSeconds = 0.40f;

    /** Require this much sustained loud activity (RMS >= BargeInMinRmsThreshold) before barge-in can interrupt AI. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.01", ClampMax="2.0"))
    float BargeInMinLoudSeconds = 0.20f;

    /** Very high RMS may interrupt sooner, but still requires short active speech accumulation. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.05", ClampMax="1.0"))
    float BargeInVeryLoudRmsThreshold = 0.20f;

    /** Hard interrupt fallback: sustained active speech forces barge-in even if loud-threshold gates are not met. Set to 0 to disable. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.0", ClampMax="5.0"))
    float BargeInForceInterruptActiveSeconds = 0.0f;

    /** Brief grace period after transcript submit where only force/sustained barge-in may interrupt. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.0", ClampMax="6.0"))
    float PostTranscriptBargeInGraceSeconds = 1.20f;

    /** If true, clear queued NPC turns near the player when voice capture begins so new player speech is prioritized. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice")
    bool bCancelQueuedTurnsOnVoiceCaptureStart = true;

    /** Default mode: continuously listen and auto-transcribe completed speech segments (no push-to-talk required). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice")
    bool bAlwaysOnAutoTranscribe = true;

    /** Pre-start the Whisper worker/model in the background on BeginPlay to reduce first-transcript latency. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice")
    bool bPrewarmWhisperOnBeginPlay = true;

    /** RMS threshold to start a speech segment in always-on mode. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.001", ClampMax="1.0"))
    float AutoTranscribeStartRmsThreshold = 0.040f;

    /** RMS threshold to continue speech segment in always-on mode. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.001", ClampMax="1.0"))
    float AutoTranscribeContinueRmsThreshold = 0.026f;

    /** Speech must remain above start threshold for this long before an always-on segment starts. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.01", ClampMax="2.0"))
    float AutoTranscribeStartHoldSeconds = 0.18f;

    /** Silence time required to end speech segment and trigger transcription. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.10", ClampMax="3.0"))
    float AutoTranscribeSilenceSeconds = 0.45f;

    /** Minimum utterance length (seconds) required to submit transcription. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.10", ClampMax="5.0"))
    float AutoTranscribeMinSpeechSeconds = 0.25f;

    /** Hard cap for a single utterance in always-on mode. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="1.0", ClampMax="30.0"))
    float AutoTranscribeMaxSpeechSeconds = 8.0f;

    /** Require at least this much speech-active audio inside a segment before sending to Whisper. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.01", ClampMax="5.0"))
    float AutoTranscribeMinActiveSpeechSeconds = 0.45f;

    /** Minimum speech-active ratio for a segment to be considered voice rather than impulse/background noise. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.01", ClampMax="1.0"))
    float AutoTranscribeMinActiveRatio = 0.50f;

    /** If true, apply a speech fence immediately when always-on segment starts. Keep off to avoid noise suppressing AI. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice")
    bool bFenceDuringAlwaysOnSegment = false;

    /** If true, barge-in only interrupts when nearby AI audio is actively playing. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice")
    bool bBargeInRequiresAudibleAIVoice = true;

    /** If true, always-on segments overlapping AI speech are dropped unless a real barge-in interrupt was triggered. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice")
    bool bDropAIAudibleSegmentsWithoutBargeIn = true;

    /** If true, estimate a rolling mic noise floor and raise thresholds dynamically in noisy rooms. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice")
    bool bUseAdaptiveNoiseFloor = true;

    /** Smoothing factor for adaptive noise floor (higher adapts faster). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.001", ClampMax="1.0"))
    float AdaptiveNoiseFloorSmoothing = 0.06f;

    /** Clamp range for adaptive noise floor RMS estimate. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.001", ClampMax="0.20"))
    float AdaptiveNoiseFloorMinRms = 0.006f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.005", ClampMax="0.30"))
    float AdaptiveNoiseFloorMaxRms = 0.06f;

    /** Dynamic threshold multipliers applied to noise floor estimate. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="1.0", ClampMax="8.0"))
    float AdaptiveStartThresholdMultiplier = 3.4f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="1.0", ClampMax="8.0"))
    float AdaptiveContinueThresholdMultiplier = 2.4f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="1.0", ClampMax="10.0"))
    float AdaptiveBargeInThresholdMultiplier = 4.8f;

    /** Upper bound for effective adaptive start threshold so speech cannot get fully locked out in noisy scenes. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.020", ClampMax="0.30"))
    float MaxEffectiveStartRmsThreshold = 0.075f;

    /** Upper bound for effective adaptive continue threshold (kept below start to maintain hysteresis). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.010", ClampMax="0.30"))
    float MaxEffectiveContinueRmsThreshold = 0.050f;

    /** Seconds to keep Director-level player priority active after a loud barge-in interrupt. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.25", ClampMax="20.0"))
    float BargeInPriorityHoldSeconds = 4.0f;

    /** Seconds to keep player priority active after an always-on segment is sent to Whisper. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.25", ClampMax="30.0"))
    float PostSegmentPriorityHoldSeconds = 3.0f;

    /** While always-on capture is actively hearing speech, hold short player priority so NPC turns don't race ahead of pending transcript context. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.0", ClampMax="12.0"))
    float ActiveSegmentPriorityHoldSeconds = 0.75f;

    /** Refresh cadence for active-segment player priority hold (lower keeps tighter suppression during speech). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.10", ClampMax="5.0"))
    float ActiveSegmentPriorityRefreshSeconds = 0.60f;

    /** Seconds to keep player priority active when a transcript is submitted to nearby AI. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.25", ClampMax="20.0"))
    float PostTranscriptPriorityHoldSeconds = 4.0f;

    /** Hard fence max hold while player speech is being captured/transcribed (failsafe timeout). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="2.0", ClampMax="120.0"))
    float PlayerSpeechFenceMaxSeconds = 8.0f;

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

    /** Reject transcripts that strongly overlap very recent nearby NPC lines (likely speaker bleed/echo). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice")
    bool bRejectLikelyNpcEchoTranscripts = true;

    /** Overlap threshold (0-1) used by NPC-echo rejection. Higher = stricter match required. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.50", ClampMax="1.0"))
    float NpcEchoSimilarityThreshold = 0.72f;

    /** Number of most recent nearby NPC lines to compare against when detecting echo transcripts. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="1", ClampMax="12"))
    int32 NpcEchoRecentNpcLines = 4;

    /** Reject low-diversity transcripts that look like ASR hallucination/noise loops. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice")
    bool bRejectLowQualityTranscripts = true;

    /** Minimum unique-token ratio for longer transcripts before they are treated as low-quality. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat|Voice", meta=(ClampMin="0.20", ClampMax="1.0"))
    float MinTranscriptUniqueTokenRatio = 0.45f;

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

#if WITH_DEV_AUTOMATION_TESTS
    void Test_SetMicCaptureActive(bool bActive) { bMicCaptureActive = bActive; }
    void Test_ProcessAlwaysOnChunk(const float* AudioData, int32 NumFrames, int32 NumChannels, int32 SampleRate, float Rms)
    {
        ProcessAlwaysOnAutoTranscribe(AudioData, NumFrames, NumChannels, SampleRate, Rms);
    }
    void Test_SetAlwaysOnSegmentHadAudibleAI(bool bValue) { bAutoSpeechSegmentHadAudibleAI = bValue; }
    void Test_SetAlwaysOnSegmentBargeInTriggered(bool bValue) { bAutoSpeechSegmentBargeInTriggered = bValue; }
    bool Test_IsAlwaysOnSegmentActive() const { return bAutoSpeechSegmentActive; }
    bool Test_IsAutoTranscribeRequestInFlight() const { return bAutoTranscribeRequestInFlight; }
#endif

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
    bool IsTranscriptLikelyNpcEcho(
        const FString& InText,
        const ULocalTalkConversationSubsystem* Sub,
        const TArray<ULocalCharacterComponent*>& CandidateTargets,
        FString& OutReason) const;
    bool IsTranscriptLikelyLowQuality(const FString& InText, FString& OutReason) const;

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
    bool HasNearbyAudibleAISpeech() const;

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
    bool bAutoSpeechSegmentHadAudibleAI = false;
    bool bAutoTranscribeRequestInFlight = false;
    double AutoSpeechStartWorldSeconds = 0.0;
    double AutoSpeechLastActiveWorldSeconds = 0.0;
    int32 AutoSpeechSampleRate = 0;
    int32 AutoSpeechNumChannels = 0;
    int32 AutoSpeechTotalFrames = 0;
    int32 AutoSpeechActiveFrames = 0;
    int32 AutoSpeechLoudFrames = 0;
    int32 AutoSpeechStartGateFrames = 0;
    TArray<int16> AutoSpeechPcm16;
    float AdaptiveNoiseFloorRms = 0.0f;
    double LastActiveSegmentPriorityApplyWorldSeconds = -1.0;

    /** World time when we last submitted a transcript; used to skip barge-in briefly so the AI can respond. */
    double LastTranscriptSubmitWorldSeconds = 0.0;
    int32 LastSingleTargetRouteIndex = INDEX_NONE;
};
