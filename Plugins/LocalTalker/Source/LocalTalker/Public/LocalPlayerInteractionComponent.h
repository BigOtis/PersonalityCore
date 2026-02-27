#pragma once

#include "CoreMinimal.h"
#include "AudioCaptureCore.h"
#include "Components/ActorComponent.h"
#include "LocalTalkerTypes.h"
#include "HAL/ThreadSafeBool.h"
#include "LocalPlayerInteractionComponent.generated.h"

class ULocalCharacterComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLocalPlayerTranscriptionEvent, const FString&, Text);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLocalPlayerInteractionErrorEvent, const FString&, Error);

/**
 * Player-side helper for interacting with nearby LocalTalk AI characters.
 * This component is intentionally separate from ULocalCharacterComponent.
 */
UCLASS(ClassGroup=(LocalTalk), meta=(BlueprintSpawnableComponent, DisplayName="LocalTalk Player Interaction"))
class LOCALTALKER_API ULocalPlayerInteractionComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    ULocalPlayerInteractionComponent();
    virtual ~ULocalPlayerInteractionComponent() override;

    /** Default max range for nearest-AI lookups. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker")
    float InteractionRange = 1500.0f;

    // If true, component uses Project Settings -> LocalTalker microphone defaults.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Microphone")
    bool bUseProjectSettingsMicInput = true;

    // Device selection mode for player microphone input (when not using project defaults).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Microphone")
    ELocalTalkMicInputDeviceMode MicInputDeviceMode = ELocalTalkMicInputDeviceMode::DefaultSystem;

    // Microphone device name to use when MicInputDeviceMode is Specific Device.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LocalTalker|Microphone", meta=(GetOptions="GetMicInputDeviceOptions"))
    FString MicInputDeviceName;

    // Raised when a Whisper transcription is produced.
    UPROPERTY(BlueprintAssignable, Category="LocalTalker|STT")
    FLocalPlayerTranscriptionEvent OnWhisperTranscription;

    // Raised for capture/transcription errors.
    UPROPERTY(BlueprintAssignable, Category="LocalTalker|STT")
    FLocalPlayerInteractionErrorEvent OnWhisperError;

    /** Finds the nearest registered AI talker within range. */
    UFUNCTION(BlueprintCallable, Category="LocalTalker")
    ULocalCharacterComponent* FindNearestAI(float MaxRange = -1.0f) const;

    /** Sends player text to a specific AI; returns false if target/text is invalid. */
    UFUNCTION(BlueprintCallable, Category="LocalTalker")
    bool SpeakToAI(ULocalCharacterComponent* TargetAI, const FString& PlayerText) const;

    /** Convenience: find nearest AI and send player text. */
    UFUNCTION(BlueprintCallable, Category="LocalTalker")
    bool SpeakToNearestAI(const FString& PlayerText, float MaxRange = -1.0f) const;

    /** Enumerates currently available audio input devices by display name. */
    UFUNCTION()
    TArray<FString> GetMicInputDeviceOptions() const;

    /** Returns the resolved microphone input device name, or empty string for system default. */
    UFUNCTION(BlueprintCallable, Category="LocalTalker|Microphone")
    FString GetResolvedMicInputDeviceName() const;

    /** Starts microphone capture for STT. */
    UFUNCTION(BlueprintCallable, Category="LocalTalker|STT")
    bool StartMicrophoneCapture();

    /** Stops microphone capture and transcribes asynchronously with Whisper. */
    UFUNCTION(BlueprintCallable, Category="LocalTalker|STT")
    bool StopMicrophoneCaptureAndTranscribe(bool bSendToNearestAI = true, float MaxRange = -1.0f);

    /** Transcribes provided PCM16 audio buffer asynchronously with Whisper. C++ use (not Blueprint-exposed). */
    bool TranscribePcm16Buffer(const TArray<int16>& Pcm16Interleaved, int32 SampleRate, int32 NumChannels, bool bSendToNearestAI = true, float MaxRange = -1.0f);

    /** Starts the Whisper worker/model warmup in the background to reduce first-transcribe latency. */
    UFUNCTION(BlueprintCallable, Category="LocalTalker|STT")
    void PrimeWhisperWorkerAsync();

    /** Cancels active microphone capture without transcription. */
    UFUNCTION(BlueprintCallable, Category="LocalTalker|STT")
    void CancelMicrophoneCapture();

    /** Returns true while microphone capture is active. */
    UFUNCTION(BlueprintCallable, Category="LocalTalker|STT")
    bool IsMicrophoneCaptureActive() const;

protected:
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    bool ResolveMicDeviceIndex(int32& OutDeviceIndex, FString& OutResolvedName, FString& OutError) const;
    bool EnsureWhisperWorker(FString& OutError);
    bool PreloadWhisperModelWithWorker(FString& OutError);
    void ShutdownWhisperWorker();
    bool TranscribeWavFileWithWhisper(const FString& WavPath, FString& OutText, FString& OutError);
    bool QueueTranscriptionFromPcm(TArray<int16>&& CapturedInterleavedPcm16, int32 SampleRate, int32 NumChannels, bool bSendToNearestAI, float MaxRange);

    Audio::FAudioCapture MicCapture;
    mutable FCriticalSection CaptureMutex;
    TArray<int16> CapturedPcm16;
    int32 CapturedSampleRate = 0;
    int32 CapturedNumChannels = 0;
    FThreadSafeBool bMicCaptureActive = false;

};
