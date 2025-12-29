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

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLocalCharacterSpokenEvent, const FString&, Text);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLocalCharacterErrorEvent, const FString&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLocalCharacterTokenEvent, const FString&, Token);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FLocalCharacterSubtitleEvent, const FString&, Speaker, const FString&, Text);
DECLARE_MULTICAST_DELEGATE_TwoParams(FLocalCharacterSubtitleNativeEvent, const FString& /*Speaker*/, const FString& /*Text*/);

/**
 * An "Actor" in the conversation. Handles individual character speech and listens for others.
 */
UCLASS(ClassGroup=(LocalTalk), meta=(BlueprintSpawnableComponent, DisplayName="LocalTalk"))
class LOCALTALKER_API ULocalCharacterComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    ULocalCharacterComponent();

    // --- Configuration ---
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    FLocalTalkerRuntimePaths PathsOverride;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    bool bUseProjectSettingsPaths = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    FLocalTalkerCharacterConfig CharacterConfigOverride;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    bool bUseProjectSettingsConfig = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Streaming TTS")
    int32 MinCharsBeforeSpeak = 24;

    // If true, the component will begin TTS while tokens stream in (sentence-chunking).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Streaming TTS")
    bool bSpeakStreaming = true;

    // Safety cap to avoid unbounded buffering if the model doesn't emit terminators.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Streaming TTS", meta=(ClampMin="16"))
    int32 MaxSentenceChars = 240;

    // Procedural audio can keep "playing" with a silent tail; this controls how quickly we force-stop it
    // once we know no more chunks are coming, so the Director can hand off the turn promptly.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Streaming TTS", meta=(ClampMin="0.0"))
    float TurnReleaseAudioTailSeconds = 0.20f;

    // If true, uses UE SubtitleManager to display subtitles (otherwise only fires events / debug).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Subtitles")
    bool bUseUESubtitles = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Subtitles")
    FString SpeakerName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Subtitles")
    bool bShowOnScreenSubtitles = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Conversation")
    float ConversationRadius = 1500.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Prompt")
    FString Directions;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Prompt")
    FString Desc;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Debug")
    bool bTraceConversation = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Debug")
    bool bDebugPrintGeneratedText = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Debug")
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
    bool IsAudioPlaying() const { return AudioComp && AudioComp->IsPlaying(); }

#if WITH_EDITOR
    // Test Helpers
    void Test_SetPendingCounts(int32 S, int32 A) { PendingSentenceCount.Set(S); PendingAudioChunkCount.Set(A); }
    void Test_SetLLMTextBuffer(const FString& S) { LLMTextBuffer = S; }
    void Test_TickComponent(float dt) { TickComponent(dt, LEVELTICK_All, nullptr); }
    int32 Test_GetPendingSentenceCount() const { return PendingSentenceCount.GetValue(); }
    int32 Test_GetLLMTextBufferLen() const { return LLMTextBuffer.Len(); }
    void Test_InitAudio() { EnsureAudio(); }
    void Test_EnqueueAudioChunk(int32 SR, int32 NC, int32 NS) 
    { 
        (void)SR; (void)NC;
        TArray<uint8> Bytes;
        Bytes.SetNumZeroed(NS * 2);
        AudioQueue.Enqueue(MoveTemp(Bytes));
        PendingAudioChunkCount.Increment();
    }
    void Test_PumpAudio() { PumpAudioToProcedural(); }
    void Test_GetProcFormat(int32& SR, int32& NC) { if (ProcWave) { SR = ProcWave->GetSampleRateForCurrentPlatform(); NC = ProcWave->NumChannels; } }

    int32 MaxQueuedSentencesAhead = 10;
    int32 MaxQueuedAudioChunksAhead = 10;
    int32 MaxBufferedCharsWhileBackpressured = 1000;
#endif

    // --- Director Callbacks (Internal) ---
    void InternalGrantTurn(const FString& PromptOrText);
    void OnHeardSpeech(const FString& InSpeakerName, const FString& Text, bool bFromUser);

    /** Returns true if this agent is currently speaking or processing LLM/TTS. */
    bool IsBusy() const { return bIsSpeakingInternal || (bLLMFinished == false && ActiveLLM != nullptr); }

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
    USoundWaveProcedural* ProcWave = nullptr;

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
    TQueue<TArray<uint8>, EQueueMode::Mpsc> AudioQueue;

    FThreadSafeBool bTTSStop = false;
    FRunnableThread* TTSThread = nullptr;
    FRunnable* TTSRunnable = nullptr;
    bool bTTSWorkerRunning = false;

    // Bookkeeping
    FThreadSafeCounter PendingSentenceCount;
    FThreadSafeCounter PendingAudioChunkCount;
    FThreadSafeBool bAudioQueueDrained = false;
    bool bNotifiedSubsystemFinished = false;
    bool bAudioStarted = false;

    bool bForce2DAudio = true;
    int32 ProcNumChannels = 1;
    int32 ProcSampleRate = 22050;

    // When showing real UE subtitles from Piper audio, this is the priority passed to SubtitleManager.
    float UESubtitlePriority = 1000.0f;

    // When printing debug subtitles on screen.
    float OnScreenSubtitleSeconds = 4.0f;

    // Voice selection (Id in Project Settings -> LocalTalker -> Voices).
    FName VoiceId = NAME_None;

    // Updated from the TTS worker thread; read on game thread.
    TAtomic<uint64> LastAudioEnqueueCycles { 0 };

    // Game-thread estimate of when the currently queued procedural audio should finish playing (world seconds).
    // Used to avoid force-stopping real speech while still allowing us to recover if the audio component gets "stuck playing" on a silent tail.
    double EstimatedAudioEndWorldSeconds = 0.0;

    void EnsureAudio();
    void EnsureProcWaveFormat(int32 SampleRate, int32 NumChannels);
    void StartTTSWorker(const FLocalTalkerRuntimePaths& Paths);
    void StopTTSWorker();
    void EnqueueSentence(const FString& Sentence);
    void ExtractAndEnqueueSentences(bool bForceFlush);
    void PumpAudioToProcedural();
    FString BuildPromptWithHistory(const FLocalTalkerCharacterConfig& Config, const FString& UserText) const;
    void EmitSubtitle(const FString& Text);
    void DebugPrintLine(const FString& Line, float Seconds, bool bNewLine) const;
    TArray<FString> GetVoiceOptions() const;
    FString ResolveVoiceOnnxPath() const;

    UFUNCTION() void HandleLLMError(const FString& Error);
    UFUNCTION() void HandleLLMToken(const FString& Token);
    UFUNCTION() void HandleLLMDelta(const FString& Text);
    UFUNCTION() void HandleLLMCompleted(const FString& Text);

    void RunPiperSentenceToAudioQueue(const FString& Sentence, const FLocalTalkerRuntimePaths& Paths, FString& OutErr);
    FLocalTalkerRuntimePaths ResolvePaths() const;
    FLocalTalkerCharacterConfig ResolveConfig() const;
};
