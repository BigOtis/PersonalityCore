#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "LocalTalkerTypes.h"
#include "Sound/SoundWaveProcedural.h"
#include "Components/AudioComponent.h"
#include "Containers/Queue.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/ThreadSafeCounter.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "LocalCharacterComponent.generated.h"

class ULocalTalkerInProcGenerateAsync;
class FLocalTalkerTTSWorker;
struct FLocalTalkMessage;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLocalCharacterSpokenEvent, const FString&, Text);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLocalCharacterErrorEvent, const FString&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLocalCharacterTokenEvent, const FString&, Token);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FLocalCharacterSubtitleEvent, const FString&, Speaker, const FString&, Text);
DECLARE_MULTICAST_DELEGATE_TwoParams(FLocalCharacterSubtitleNativeEvent, const FString& /*Speaker*/, const FString& /*Text*/);

USTRUCT()
struct FLocalTalkPendingAudioPayload
{
    GENERATED_BODY()

    UPROPERTY()
    TObjectPtr<USoundWaveProcedural> Wave = nullptr;

    UPROPERTY()
    FString SubtitleText;

    UPROPERTY()
    float SubtitleDurationSeconds = 0.0f;

    UPROPERTY()
    FString RequestId;
};

/**
 * An "Actor" in the conversation. Handles individual character speech and listens for others.
 */
UCLASS(ClassGroup=(LocalTalk), meta=(BlueprintSpawnableComponent, DisplayName="LocalTalk"))
class LOCALTALKER_API ULocalCharacterComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    ULocalCharacterComponent();
    virtual ~ULocalCharacterComponent() override;
    static void ShutdownSharedTtsWorkerGlobal();

    // --- Configuration ---
    UPROPERTY(EditAnywhere, Category="LocalTalker")
    FLocalTalkerRuntimePaths PathsOverride;

    UPROPERTY(EditAnywhere, Category="LocalTalker")
    bool bUseProjectSettingsPaths = true;

    UPROPERTY(EditAnywhere, Category="LocalTalker")
    FLocalTalkerCharacterConfig CharacterConfigOverride;

    UPROPERTY(EditAnywhere, Category="LocalTalker")
    bool bUseProjectSettingsConfig = true;

    UPROPERTY(EditAnywhere, Category="LocalTalker|Streaming TTS")
    int32 MinCharsBeforeSpeak = 24;

    // If true, allow short phrase chunks (commas/clauses) for lower latency streaming.
    UPROPERTY(EditAnywhere, Category="LocalTalker|Streaming TTS")
    bool bAllowPhraseChunks = true;

    // Minimum word count before we allow phrase chunking.
    UPROPERTY(EditAnywhere, Category="LocalTalker|Streaming TTS", meta=(ClampMin="1"))
    int32 MinWordsBeforeSpeak = 6;

    // If set, force a chunk once this many words are buffered.
    UPROPERTY(EditAnywhere, Category="LocalTalker|Streaming TTS", meta=(ClampMin="1"))
    int32 MaxWordsBeforeSpeak = 16;

    // If true, the component will begin TTS while tokens stream in (sentence-chunking).
    UPROPERTY(EditAnywhere, Category="LocalTalker|Streaming TTS")
    bool bSpeakStreaming = false;

    // Safety cap to avoid unbounded buffering if the model doesn't emit terminators.
    UPROPERTY(EditAnywhere, Category="LocalTalker|Streaming TTS", meta=(ClampMin="16"))
    int32 MaxSentenceChars = 240;

    // If true, uses UE SubtitleManager to display subtitles (otherwise only fires events / debug).
    UPROPERTY(EditAnywhere, Category="LocalTalker|Subtitles")
    bool bUseUESubtitles = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Subtitles")
    FString SpeakerName;

    UPROPERTY(EditAnywhere, Category="LocalTalker|Subtitles")
    bool bShowOnScreenSubtitles = true;

    UPROPERTY(EditAnywhere, Category="LocalTalker|Voices", meta=(GetOptions="GetVoiceOptions"))
    FName VoiceId = NAME_None;

    // If true, component uses Project Settings -> LocalTalker microphone defaults.
    UPROPERTY(EditAnywhere, Category="LocalTalker|Microphone")
    bool bUseProjectSettingsMicInput = true;

    // Device selection mode for player microphone input (when not using project defaults).
    UPROPERTY(EditAnywhere, Category="LocalTalker|Microphone")
    ELocalTalkMicInputDeviceMode MicInputDeviceMode = ELocalTalkMicInputDeviceMode::DefaultSystem;

    // Microphone device name to use when MicInputDeviceMode is Specific Device.
    UPROPERTY(EditAnywhere, Category="LocalTalker|Microphone", meta=(GetOptions="GetMicInputDeviceOptions"))
    FString MicInputDeviceName;

    UPROPERTY(EditAnywhere, Category="LocalTalker|Conversation")
    float ConversationRadius = 1500.0f;

    UPROPERTY(EditAnywhere, Category="LocalTalker|Audio")
    bool bUseLocalSound = true;

    UPROPERTY(EditAnywhere, Category="LocalTalker|Audio", meta=(ClampMin="0.0"))
    float VoiceVolumeMultiplier = 1.0f;

    UPROPERTY(EditAnywhere, Category="LocalTalker|Audio", meta=(ClampMin="0.0"))
    float VoiceAttenuationRadius = 0.0f;

    UPROPERTY(EditAnywhere, Category="LocalTalker|Audio", meta=(ClampMin="0.0"))
    float AudioCompletionGraceSeconds = 0.25f;

    UPROPERTY(EditAnywhere, Category="LocalTalker|Prompt")
    FString Directions;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Prompt")
    FString Desc;

    UPROPERTY(EditAnywhere, Category="LocalTalker|Debug")
    bool bTraceConversation = false;

    UPROPERTY(EditAnywhere, Category="LocalTalker|Debug")
    bool bDebugPrintGeneratedText = false;

    UPROPERTY(EditAnywhere, Category="LocalTalker|Debug")
    bool bDebugLogTokens = false;

    // --- Events ---
    UPROPERTY(BlueprintAssignable, Category="LocalTalker")
    FLocalCharacterSpokenEvent OnSpokenText;

    UPROPERTY(BlueprintAssignable, Category="LocalTalker")
    FLocalCharacterErrorEvent OnError;

    UPROPERTY(BlueprintAssignable, Category="LocalTalker")
    FLocalCharacterTokenEvent OnToken;

    UPROPERTY(BlueprintAssignable, Category="LocalTalker")
    FLocalCharacterSubtitleEvent OnSubtitle;

    // Native-only hook (usable from C++ without UFUNCTION bindings). Fired alongside OnSubtitle.
    FLocalCharacterSubtitleNativeEvent OnSubtitleNative;

    // --- API ---
    /** Request to speak a prompt (will be queued by Director). */
    UFUNCTION(BlueprintCallable, Category="LocalTalker")
    void SendPromptAndSpeakStreamingInProc(const FString& Prompt);

    /** Request to speak raw text (will be queued by Director). */
    UFUNCTION(BlueprintCallable, Category="LocalTalker")
    void SpeakTextLocal(const FString& Text);

    /** Stop talking immediately. */
    UFUNCTION(BlueprintCallable, Category="LocalTalker")
    void Interrupt();

    /** Clear conversation history. */
    UFUNCTION(BlueprintCallable, Category="LocalTalker|Prompt")
    void ClearConversation();

    FString GetSpeakerNameResolved() const;
    float GetHearingRadius() const
    {
        return (VoiceAttenuationRadius > 0.0f) ? VoiceAttenuationRadius : ConversationRadius;
    }
    bool IsAudioPlaying() const { return AudioComp && AudioComp->IsPlaying(); }
    bool IsGenerationBusy() const
    {
        const bool bLLMBusy = (bLLMFinished == false && ActiveLLM != nullptr);
        const bool bTTSBusy = (PendingSentenceCount.GetValue() > 0);
        return bIsSpeakingInternal || bLLMBusy || bTTSBusy;
    }

#if WITH_EDITOR
    // Test Helpers
    void Test_SetPendingCounts(int32 S) { PendingSentenceCount.Set(S); }
    void Test_SetLLMTextBuffer(const FString& S) { LLMTextBuffer = S; }
    void Test_TickComponent(float dt) { TickComponent(dt, LEVELTICK_All, nullptr); }
    int32 Test_GetPendingSentenceCount() const { return PendingSentenceCount.GetValue(); }
    int32 Test_GetLLMTextBufferLen() const { return LLMTextBuffer.Len(); }
    void Test_InitAudio() { EnsureAudio(); }
    FString Test_BuildLlama3PromptFromContext(
        const TArray<FLocalTalkMessage>& ContextHistory,
        const TArray<ULocalCharacterComponent*>& ContextParticipants,
        const FString& TurnPrompt) const;

    int32 MaxQueuedSentencesAhead = 10;
    int32 MaxBufferedCharsWhileBackpressured = 1000;
#endif

    // --- Director Callbacks (Internal) ---
    void InternalGrantTurn(const FString& PromptOrText);
    void OnHeardSpeech(const FString& InSpeakerName, const FString& Text, bool bFromUser);

    /** Returns true if this agent is currently speaking or processing LLM/TTS. */
    bool IsBusy() const
    {
        return IsGenerationBusy() || !bAudioPlaybackComplete || ((AudioComp != nullptr) && AudioComp->IsPlaying());
    }

    UFUNCTION()
    TArray<FString> GetVoiceOptions() const;

    // Enumerates currently available audio input devices by display name.
    UFUNCTION()
    TArray<FString> GetMicInputDeviceOptions() const;

    // Returns the resolved microphone input device name, or empty string for system default.
    UFUNCTION(BlueprintCallable, Category="LocalTalker|Microphone")
    FString GetResolvedMicInputDeviceName() const;

    bool IsAudioPlaybackComplete() const { return bAudioPlaybackComplete; }

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
    friend class FLocalTalkerTTSWorker;
    friend class ULocalTalkConversationSubsystem;

    UPROPERTY()
    UAudioComponent* AudioComp = nullptr;

    UPROPERTY()
    ULocalTalkerInProcGenerateAsync* ActiveLLM = nullptr;

    FThreadSafeBool bInterrupted = false;
    FThreadSafeBool bLLMFinished = false;
    FThreadSafeBool bIsSpeakingInternal = false;
    bool bSpokeThisTurn = false;

    FString LLMTextBuffer;
    FString LLMFullText;
    double LastTextAppendSeconds = 0.0;

    // If tokens stop streaming (network/model stall), flush whatever we have so TTS can finish.
    float FlushSeconds = 0.40f;
    
    TQueue<FString, EQueueMode::Mpsc> SentenceQueue;

    FThreadSafeBool bTTSStop = false;
    FRunnableThread* TTSThread = nullptr;
    FRunnable* TTSRunnable = nullptr;
    bool bTTSWorkerRunning = false;

    // Bookkeeping
    FThreadSafeCounter PendingSentenceCount;
    bool bNotifiedSubsystemFinished = false;
    FThreadSafeBool bAudioPlaybackComplete = true;

    // Priority passed to SubtitleManager when using generated speech audio.
    float UESubtitlePriority = 1000.0f;

    // When printing debug subtitles on screen.
    float OnScreenSubtitleSeconds = 4.0f;

    // Pending playback (waiting for another speaker to finish).
    UPROPERTY()
    USoundWaveProcedural* PendingAudioWave = nullptr;
    FString PendingAudioRequestId;
    FString PendingSubtitleText;
    float PendingSubtitleDurationSeconds = 0.0f;
    UPROPERTY()
    TArray<FLocalTalkPendingAudioPayload> PendingAudioPayloads;
    FString ActiveAudioRequestId;
    double ActiveAudioStartWorldSeconds = 0.0;
    float ActiveAudioDurationSeconds = 0.0f;
    FThreadSafeCounter AudioRequestEpoch;

    // Tracks whether this speaker is queued behind another (event-driven, not polled).
    bool bWaitingForOtherSpeaker = false;
    double BlockedSinceSeconds = 0.0;

    void EnsureAudio();
    void ResetPendingAudioState();
    void QueuePendingAudioPayload(
        USoundWaveProcedural* Wave,
        const FString& SubtitleText,
        float SubtitleDurationSeconds,
        const FString& RequestId,
        int32 RequestEpoch);
    void ApplyAudioDurationForRequest(
        const FString& RequestId,
        float DurationSeconds,
        int32 RequestEpoch);
    void StartTTSWorker(const FLocalTalkerRuntimePaths& Paths);
    void StopTTSWorker();
    void EnqueueSentence(const FString& Sentence);
    void EnqueueSentenceInternal(const FString& Sentence, bool bBroadcast);
    void ExtractAndEnqueueSentences(bool bForceFlush);
    void TryStartPendingAudio();
    bool IsAudioBlockedByOtherSpeaker() const;
    FString BuildPromptWithHistory(const FLocalTalkerCharacterConfig& Config, const FString& UserText) const;
    void EmitSubtitle(const FString& Text, float DurationSeconds);
    void DebugPrintLine(const FString& Line, float Seconds, bool bNewLine) const;
    bool ShouldAllowTalk() const;
    UFUNCTION()
    void HandleAudioFinished();

    UFUNCTION() void HandleLLMError(const FString& Error);
    UFUNCTION() void HandleLLMToken(const FString& Token);
    UFUNCTION() void HandleLLMDelta(const FString& Text);
    UFUNCTION() void HandleLLMCompleted(const FString& Text);

    // Kokoro ONNX TTS (CPU backend)
    void RunKokoroSentenceToAudio(const FString& Sentence, const FLocalTalkerRuntimePaths& Paths, FString& OutErr);
    bool EnsureKokoroWorker(const FLocalTalkerRuntimePaths& Paths, FString& OutErr);
    bool ResolveKokoroVoiceSelection(FString& OutVoice) const;
    void ShutdownKokoroWorker();

    FLocalTalkerRuntimePaths ResolvePaths() const;
    FLocalTalkerCharacterConfig ResolveConfig() const;
};
