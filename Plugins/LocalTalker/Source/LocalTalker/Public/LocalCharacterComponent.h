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

UCLASS(ClassGroup=(LocalTalk), meta=(BlueprintSpawnableComponent, DisplayName="LocalTalk"))
class LOCALTALKER_API ULocalCharacterComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    ULocalCharacterComponent();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    FLocalTalkerRuntimePaths PathsOverride;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    bool bUseProjectSettingsPaths = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    FLocalTalkerCharacterConfig CharacterConfigOverride;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    bool bUseProjectSettingsConfig = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Streaming TTS")
    bool bSpeakStreaming = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Streaming TTS")
    int32 MinCharsBeforeSpeak = 24;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Streaming TTS")
    float FlushSeconds = 0.45f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Streaming TTS")
    int32 MaxSentenceChars = 280;

    // Backpressure: limit how far ahead we get of playback. If too much is queued,
    // we stop extracting/enqueueing new sentences until audio catches up.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Streaming TTS", meta=(ClampMin="0", UIMin="0"))
    int32 MaxQueuedSentencesAhead = 2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Streaming TTS", meta=(ClampMin="0", UIMin="0"))
    int32 MaxQueuedAudioChunksAhead = 3;

    // Safety: cap how much text we buffer while we’re waiting for TTS/audio (prevents runaway memory use).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Streaming TTS", meta=(ClampMin="0", UIMin="0"))
    int32 MaxBufferedCharsWhileBackpressured = 8000;

    UPROPERTY(BlueprintAssignable, Category="LocalTalker")
    FLocalCharacterSpokenEvent OnSpokenText;

    UPROPERTY(BlueprintAssignable, Category="LocalTalker")
    FLocalCharacterErrorEvent OnError;

    UPROPERTY(BlueprintAssignable, Category="LocalTalker")
    FLocalCharacterTokenEvent OnToken;

    // Fired for each sentence/chunk that the character is about to speak (use this for UI subtitles).
    UPROPERTY(BlueprintAssignable, Category="LocalTalker")
    FLocalCharacterSubtitleEvent OnSubtitle;

    // Optional display name for subtitles/debug. If empty, we'll fall back to the owning actor name.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Subtitles")
    FString SpeakerName;

    // If true, we'll show simple on-screen subtitles like "Name: text" (good for quick debugging).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Subtitles")
    bool bShowOnScreenSubtitles = true;

    // Duration (seconds) to keep the on-screen subtitle line.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Subtitles")
    float OnScreenSubtitleSeconds = 4.0f;

    // Use Unreal's built-in subtitle renderer (requires subtitles enabled in game settings).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Subtitles")
    bool bUseUESubtitles = true;

    // Higher values win when multiple characters speak at once.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Subtitles")
    float UESubtitlePriority = 1000.0f;

    // Default prompt parts (editable). These are combined with conversation history each turn.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Prompt")
    FString DirectionsPrompt;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Prompt")
    FString CharacterDescriptionPrompt;

    // Conversation history length controls (in addition to Config overrides).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Prompt")
    bool bUseConversationHistory = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Prompt", meta=(ClampMin="0", UIMin="0"))
    int32 MaxContextChars = 1600;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Prompt", meta=(ClampMin="0", UIMin="0"))
    int32 MaxHistoryMessages = 16;

    // Selected TTS voice (Piper). If None, uses the first valid voice from Project Settings -> LocalTalker -> Voices.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Voices", meta=(GetOptions=GetVoiceOptions))
    FName VoiceId = NAME_None;

    UFUNCTION()
    TArray<FString> GetVoiceOptions() const;

    // Proximity conversation settings
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Conversation")
    bool bEnableProximityConversation = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Conversation", meta=(ClampMin="0.0", UIMin="0.0"))
    float ConversationRadius = 1500.0f;

    // If true, this character will automatically respond when nearby characters finish speaking.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Conversation")
    bool bAutoRespondToHeardSpeech = true;

    // Minimum time between this character taking turns (seconds).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Conversation", meta=(ClampMin="0.0", UIMin="0.0"))
    float MinSecondsBetweenTurns = 0.75f;

    // Additional "thinking" delay before responding (randomized between min/max).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Conversation", meta=(ClampMin="0.0", UIMin="0.0"))
    float ThinkingDelayMin = 0.25f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Conversation", meta=(ClampMin="0.0", UIMin="0.0"))
    float ThinkingDelayMax = 0.9f;

    // How likely this character is to respond to OTHER NPCs (0-1). Lower = less chatter.
    // Note: player/user utterances always get a response (the subsystem bypasses this gate for user turns).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Conversation", meta=(ClampMin="0.0", ClampMax="1.0", UIMin="0.0", UIMax="1.0"))
    float ResponseLikelihood = 0.85f;

    // Priority boost when responding to user input (vs other NPCs).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Conversation", meta=(ClampMin="0.0", UIMin="0.0"))
    float UserResponsePriorityBoost = 2.0f;

    // If enabled, this character may initiate a line after a long silence (ambient chatter).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Idle Chatter")
    bool bEnableIdleChatter = true;

    // Minimum silence time before this character may initiate idle chatter.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Idle Chatter", meta=(ClampMin="0.0", UIMin="0.0"))
    float IdleChatterMinSilenceSeconds = 10.0f;

    // Maximum silence time before this character may initiate idle chatter.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Idle Chatter", meta=(ClampMin="0.0", UIMin="0.0"))
    float IdleChatterMaxSilenceSeconds = 25.0f;

    // Optional prompt used when initiating idle chatter. If empty, a safe default is used.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Idle Chatter")
    FString IdleChatterPrompt;

    // Print generated text/tokens to the screen for debugging.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Debug")
    bool bDebugPrintGeneratedText = true;

    // Log each token to Output Log (can be very noisy).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Debug")
    bool bDebugLogTokens = false;

    // If true, force this component's audio to be non-spatial (2D/UI) so you can always hear it.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Audio")
    bool bForce2DAudio = true;

    UFUNCTION(BlueprintCallable, Category="LocalTalker")
    void Interrupt();

    UFUNCTION(BlueprintCallable, Category="LocalTalker", meta=(DisplayName="LocalTalk Speak Text (Local)"))
    void SpeakTextLocal(const FString& Text);

    UFUNCTION(BlueprintCallable, Category="LocalTalker", meta=(DisplayName="LocalTalk Send Prompt And Speak (Streaming, In-Proc)"))
    void SendPromptAndSpeakStreamingInProc(const FString& Prompt);

    // Clears this character's conversation history.
    UFUNCTION(BlueprintCallable, Category="LocalTalker|Prompt")
    void ClearConversation();

    // Used by the proximity conversation system to update context when another character speaks.
    UFUNCTION(BlueprintCallable, Category="LocalTalker|Conversation")
    void ReceiveBroadcastSpeech(const FString& InSpeakerName, const FString& Text, bool bFromUser);

    UFUNCTION(BlueprintCallable, Category="LocalTalker|Conversation")
    FString GetResolvedSpeakerName() const;

    // Returns true if this character is currently speaking (LLM generating or audio playing)
    UFUNCTION(BlueprintCallable, Category="LocalTalker|Conversation")
    bool IsSpeaking() const;

    // Returns true if audio is still playing for this character
    UFUNCTION(BlueprintCallable, Category="LocalTalker|Conversation")
    bool IsAudioPlaying() const;

    // Convenience wrapper: broadcast user input near this actor. Can interrupt others mid-sentence.
    UFUNCTION(BlueprintCallable, Category="LocalTalker|Conversation")
    void SendUserTextInterrupt(const FString& UserText, bool bInterrupt = true);

#if WITH_DEV_AUTOMATION_TESTS
    // ---------------------------------------------------------------------
    // Test helpers (compiled only for dev automation tests)
    // ---------------------------------------------------------------------
    void Test_SetLLMTextBuffer(const FString& InText);
    int32 Test_GetLLMTextBufferLen() const;
    void Test_SetPendingCounts(int32 InPendingSentences, int32 InPendingAudioChunks);
    int32 Test_GetPendingSentenceCount() const;
    int32 Test_GetPendingAudioChunkCount() const;
    void Test_InitAudio();
    void Test_EnqueueAudioChunk(int32 InSampleRate, int32 InNumChannels, int32 InNumSamples);
    void Test_PumpAudio();
    void Test_GetProcFormat(int32& OutSampleRate, int32& OutNumChannels) const;
#endif

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
    friend class FLocalTalkerTTSWorker;

    UPROPERTY()
    UAudioComponent* AudioComp = nullptr;

    UPROPERTY()
    USoundWaveProcedural* ProcWave = nullptr;

    UPROPERTY()
    ULocalTalkerInProcGenerateAsync* ActiveLLM = nullptr;

    FThreadSafeBool bInterrupted = false;
    FThreadSafeBool bLLMFinished = false;

    FString LLMTextBuffer;
    FString LLMFullText;
    double LastTextAppendSeconds = 0.0;

    struct FChatMsg
    {
        FString Role;
        FString Content;
    };
    TArray<FChatMsg> History;

    TQueue<FString, EQueueMode::Mpsc> SentenceQueue;
    struct FAudioChunk
    {
        int32 SampleRate = 0;
        int32 NumChannels = 0;
        TArray<uint8> Bytes;
    };

    TQueue<FAudioChunk, EQueueMode::Mpsc> AudioQueue;

    FThreadSafeBool bTTSWorkerRunning = false;
    FThreadSafeBool bTTSStop = false;
    FRunnableThread* TTSThread = nullptr;
    FRunnable* TTSRunnable = nullptr;

    bool bAudioStarted = false;
    int32 ProcSampleRate = 0;
    int32 ProcNumChannels = 0;

    // Audio completion tracking
    FThreadSafeBool bAudioQueueDrained = false;     // No more audio chunks coming
    int32 TotalSamplesQueued = 0;                  // Samples queued to procedural wave (best-effort)
    double AudioPlaybackStartTime = 0.0;           // When we started playing audio
    float EstimatedAudioDuration = 0.0f;           // Estimated total duration
    bool bNotifiedAudioComplete = false;           // Already notified subsystem of completion
    double AudioStoppedTime = 0.0;                 // When audio stopped (for grace period)
    bool bAudioStoppedDetected = false;            // First detection of audio stop

    // Robust pipeline tracking (procedural audio can "gap" between chunks, so IsPlaying() is not enough)
    FThreadSafeCounter PendingSentenceCount;         // # of sentences queued but not yet consumed by TTS worker
    FThreadSafeCounter PendingAudioChunkCount;       // # of audio chunks queued but not yet pumped into ProcWave
    FThreadSafeCounter64 LastAudioChunkQueuedCycles;   // Updated on game thread when we queue bytes into ProcWave
    FThreadSafeCounter64 LastAudioChunkProducedCycles; // Updated on worker thread when we enqueue bytes into AudioQueue

    FLocalTalkerRuntimePaths ResolvePaths() const;
    FLocalTalkerCharacterConfig ResolveConfig() const;

    void EnsureAudio();
    void EnsureProcWaveFormat(int32 SampleRate, int32 NumChannels);
    void StartTTSWorker(const FLocalTalkerRuntimePaths& Paths);
    void StopTTSWorker();

    void EnqueueSentence(const FString& Sentence);
    void ExtractAndEnqueueSentences(bool bForceFlush);

    void PumpAudioToProcedural();

    FString GetSpeakerNameResolved() const;
    void EmitSubtitle(const FString& Text);
    FString BuildPromptWithHistory(const FLocalTalkerCharacterConfig& Config, const FString& UserText) const;
    void TrimHistory();
    void DebugPrintLine(const FString& Line, float Seconds = 2.0f, bool bNewLine = false) const;

    // Resolve Piper voice path from VoiceId / settings / auto-discovery.
    FString ResolveVoiceOnnxPath() const;

    UFUNCTION()
    void HandleLLMError(const FString& Error);

    UFUNCTION()
    void HandleLLMToken(const FString& Token);

    UFUNCTION()
    void HandleLLMDelta(const FString& Text);

    UFUNCTION()
    void HandleLLMCompleted(const FString& Text);

    void RunPiperSentenceToAudioQueue(const FString& Sentence, const FLocalTalkerRuntimePaths& Paths, FString& OutErr);
};
