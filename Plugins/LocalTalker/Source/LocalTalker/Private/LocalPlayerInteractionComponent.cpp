#include "LocalPlayerInteractionComponent.h"

#include "LocalCharacterComponent.h"
#include "LocalTalkConversationSubsystem.h"
#include "LocalTalkerLog.h"
#include "LocalTalkerProcess.h"
#include "LocalTalkerSettings.h"
#include "LocalTalkerWav.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectIterator.h"
#include "Templates/Atomic.h"

struct FLocalPlayerWhisperWorker
{
    FProcHandle Handle;
    FLocalProcPipes Pipes;
    FString StdoutBuffer;
    FString StderrBuffer;
};

static TUniquePtr<FLocalPlayerWhisperWorker> GLocalPlayerWhisperWorker;
static FCriticalSection GLocalPlayerWhisperWorkerMutex;
static TAtomic<bool> GLocalPlayerWhisperPrimed(false);
static TAtomic<bool> GLocalPlayerWhisperPrimeInFlight(false);

namespace
{
static FString QuoteArg(const FString& In)
{
    FString Escaped = In;
    Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
    return FString::Printf(TEXT("\"%s\""), *Escaped);
}

static bool TryPopLine(FString& InOutBuffer, FString& OutLine)
{
    int32 NewlineIndex = INDEX_NONE;
    if (!InOutBuffer.FindChar(TEXT('\n'), NewlineIndex))
    {
        return false;
    }

    OutLine = InOutBuffer.Left(NewlineIndex);
    InOutBuffer = InOutBuffer.Mid(NewlineIndex + 1);
    OutLine.ReplaceInline(TEXT("\r"), TEXT(""));
    OutLine.TrimStartAndEndInline();
    return true;
}

static bool SerializeJsonLine(const TSharedRef<FJsonObject>& Json, FString& OutLine)
{
    OutLine.Reset();
    TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutLine);
    if (!FJsonSerializer::Serialize(Json, Writer))
    {
        return false;
    }
    OutLine += TEXT("\n");
    return true;
}

static bool ParseJsonLine(const FString& Line, TSharedPtr<FJsonObject>& OutObj)
{
    OutObj.Reset();
    if (Line.IsEmpty())
    {
        return false;
    }

    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
    return FJsonSerializer::Deserialize(Reader, OutObj) && OutObj.IsValid();
}

static ELocalTalkMicInputDeviceMode ResolveMicMode(const ULocalPlayerInteractionComponent* Component, FString& OutNamedDevice)
{
    OutNamedDevice.Reset();
    if (!Component)
    {
        return ELocalTalkMicInputDeviceMode::DefaultSystem;
    }

    if (Component->bUseProjectSettingsMicInput)
    {
        if (const ULocalTalkerSettings* Settings = GetDefault<ULocalTalkerSettings>())
        {
            OutNamedDevice = Settings->MicInputDeviceName;
            return Settings->MicInputDeviceMode;
        }
    }

    OutNamedDevice = Component->MicInputDeviceName;
    return Component->MicInputDeviceMode;
}

static FLocalTalkerRuntimePaths ResolveRuntimePaths()
{
    const ULocalTalkerSettings* Settings = GetDefault<ULocalTalkerSettings>();
    FLocalTalkerRuntimePaths Paths = Settings ? Settings->DefaultPaths : FLocalTalkerRuntimePaths();

    if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LocalTalker")))
    {
        const FString Base = Plugin->GetBaseDir();
        if (Paths.WorkingDir.IsEmpty())
        {
            Paths.WorkingDir = Base;
        }
        if (Paths.WhisperWorkerScriptPath.IsEmpty())
        {
            Paths.WhisperWorkerScriptPath = FPaths::Combine(Base, TEXT("Resources/Whisper/whisper_stt_worker.py"));
        }
        if (Paths.WhisperPythonExePath.IsEmpty())
        {
            Paths.WhisperPythonExePath = TEXT("python");
        }
    }

    return Paths;
}

static TArray<int16> DownmixToMono(const TArray<int16>& Interleaved, int32 NumChannels)
{
    if (NumChannels <= 1)
    {
        return Interleaved;
    }

    const int32 NumFrames = Interleaved.Num() / NumChannels;
    TArray<int16> Mono;
    Mono.SetNumUninitialized(NumFrames);

    for (int32 Frame = 0; Frame < NumFrames; ++Frame)
    {
        int32 Sum = 0;
        const int32 Base = Frame * NumChannels;
        for (int32 Channel = 0; Channel < NumChannels; ++Channel)
        {
            Sum += static_cast<int32>(Interleaved[Base + Channel]);
        }

        const int32 Avg = Sum / NumChannels;
        Mono[Frame] = static_cast<int16>(FMath::Clamp(Avg, -32768, 32767));
    }

    return Mono;
}
} // namespace

ULocalPlayerInteractionComponent::ULocalPlayerInteractionComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

ULocalPlayerInteractionComponent::~ULocalPlayerInteractionComponent() = default;

void ULocalPlayerInteractionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    CancelMicrophoneCapture();
    ShutdownWhisperWorker();
    Super::EndPlay(EndPlayReason);
}

ULocalCharacterComponent* ULocalPlayerInteractionComponent::FindNearestAI(float MaxRange) const
{
    const AActor* Owner = GetOwner();
    UWorld* World = GetWorld();
    if (!World && Owner)
    {
        World = Owner->GetWorld();
    }
    if (!Owner || !World)
    {
        return nullptr;
    }

    const float EffectiveRange = (MaxRange > 0.0f) ? MaxRange : InteractionRange;
    const float EffectiveRangeSq = (EffectiveRange > 0.0f) ? (EffectiveRange * EffectiveRange) : TNumericLimits<float>::Max();
    const FVector OwnerLoc = Owner->GetActorLocation();

    ULocalCharacterComponent* Best = nullptr;
    float BestDistSq = TNumericLimits<float>::Max();
    ULocalCharacterComponent* BestAny = nullptr;
    float BestAnyDistSq = TNumericLimits<float>::Max();
    auto ConsiderTalker = [&](ULocalCharacterComponent* Talker)
    {
        if (!Talker || !Talker->GetOwner())
        {
            return;
        }
        if (Talker->GetOwner() == Owner)
        {
            return;
        }

        const float DistSq = FVector::DistSquared(OwnerLoc, Talker->GetOwner()->GetActorLocation());
        if (DistSq < BestAnyDistSq)
        {
            BestAnyDistSq = DistSq;
            BestAny = Talker;
        }

        if (DistSq > EffectiveRangeSq)
        {
            return;
        }

        if (DistSq < BestDistSq)
        {
            BestDistSq = DistSq;
            Best = Talker;
        }
    };

    if (ULocalTalkConversationSubsystem* Sub = World->GetSubsystem<ULocalTalkConversationSubsystem>())
    {
        for (ULocalCharacterComponent* Talker : Sub->GetRegisteredTalkers())
        {
            ConsiderTalker(Talker);
        }
    }

    if (!Best)
    {
        for (TObjectIterator<ULocalCharacterComponent> It; It; ++It)
        {
            ULocalCharacterComponent* Talker = *It;
            if (!Talker || Talker->HasAnyFlags(RF_ClassDefaultObject))
            {
                continue;
            }
            if (Talker->GetWorld() != World)
            {
                continue;
            }
            ConsiderTalker(Talker);
        }
    }

    if (!Best && !Owner->GetRootComponent() && BestAny)
    {
        return BestAny;
    }

    return Best;
}

bool ULocalPlayerInteractionComponent::SpeakToAI(ULocalCharacterComponent* TargetAI, const FString& PlayerText) const
{
    if (!TargetAI || PlayerText.IsEmpty())
    {
        return false;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        const AActor* Owner = GetOwner();
        if (Owner)
        {
            World = Owner->GetWorld();
        }
    }
    if (!World && TargetAI)
    {
        World = TargetAI->GetWorld();
    }
    if (!World)
    {
        return false;
    }

    ULocalTalkConversationSubsystem* Sub = World->GetSubsystem<ULocalTalkConversationSubsystem>();
    if (!Sub)
    {
        return false;
    }

    Sub->RequestTurn(TargetAI, PlayerText, /*bFromUser*/true);
    return true;
}

bool ULocalPlayerInteractionComponent::SpeakToNearestAI(const FString& PlayerText, float MaxRange) const
{
    ULocalCharacterComponent* Target = FindNearestAI(MaxRange);
    return SpeakToAI(Target, PlayerText);
}

TArray<FString> ULocalPlayerInteractionComponent::GetMicInputDeviceOptions() const
{
    TArray<FString> Out;
    TArray<Audio::FCaptureDeviceInfo> Devices;

    Audio::FAudioCapture Capture;
    Capture.GetCaptureDevicesAvailable(Devices);

    for (const Audio::FCaptureDeviceInfo& Device : Devices)
    {
        if (!Device.DeviceName.IsEmpty())
        {
            Out.AddUnique(Device.DeviceName);
        }
    }

    Out.Sort();
    return Out;
}

FString ULocalPlayerInteractionComponent::GetResolvedMicInputDeviceName() const
{
    FString DesiredName;
    const ELocalTalkMicInputDeviceMode Mode = ResolveMicMode(this, DesiredName);
    if (Mode == ELocalTalkMicInputDeviceMode::DefaultSystem)
    {
        return FString();
    }

    DesiredName.TrimStartAndEndInline();
    if (DesiredName.IsEmpty())
    {
        return FString();
    }

    const TArray<FString> Available = GetMicInputDeviceOptions();
    for (const FString& Name : Available)
    {
        if (Name.Equals(DesiredName, ESearchCase::IgnoreCase))
        {
            return Name;
        }
    }

    return FString();
}

bool ULocalPlayerInteractionComponent::ResolveMicDeviceIndex(int32& OutDeviceIndex, FString& OutResolvedName, FString& OutError) const
{
    OutDeviceIndex = Audio::DefaultDeviceIndex;
    OutResolvedName.Reset();
    OutError.Reset();

    FString DesiredName;
    const ELocalTalkMicInputDeviceMode Mode = ResolveMicMode(this, DesiredName);
    if (Mode == ELocalTalkMicInputDeviceMode::DefaultSystem)
    {
        return true;
    }

    DesiredName.TrimStartAndEndInline();
    if (DesiredName.IsEmpty())
    {
        return true;
    }

    TArray<Audio::FCaptureDeviceInfo> Devices;
    Audio::FAudioCapture Capture;
    Capture.GetCaptureDevicesAvailable(Devices);

    for (int32 Index = 0; Index < Devices.Num(); ++Index)
    {
        if (Devices[Index].DeviceName.Equals(DesiredName, ESearchCase::IgnoreCase))
        {
            OutDeviceIndex = Index;
            OutResolvedName = Devices[Index].DeviceName;
            return true;
        }
    }

    OutError = FString::Printf(TEXT("Requested microphone device '%s' not found; using default system input."), *DesiredName);
    return true;
}

bool ULocalPlayerInteractionComponent::StartMicrophoneCapture()
{
    if (bMicCaptureActive)
    {
        UE_LOG(LogLocalTalker, Verbose, TEXT("[STT] StartMicrophoneCapture ignored; already active."));
        return false;
    }

    if (MicCapture.IsCapturing() || MicCapture.IsStreamOpen())
    {
        MicCapture.AbortStream();
    }

    int32 DeviceIndex = Audio::DefaultDeviceIndex;
    FString ResolvedMicName;
    FString ResolveError;
    if (!ResolveMicDeviceIndex(DeviceIndex, ResolvedMicName, ResolveError))
    {
        OnWhisperError.Broadcast(ResolveError);
        return false;
    }

    if (!ResolveError.IsEmpty())
    {
        UE_LOG(LogLocalTalker, Warning, TEXT("[STT] %s"), *ResolveError);
    }

    {
        FScopeLock Lock(&CaptureMutex);
        CapturedPcm16.Reset();
        CapturedSampleRate = 0;
        CapturedNumChannels = 0;
    }

    Audio::FAudioCaptureDeviceParams Params;
    Params.DeviceIndex = DeviceIndex;
    Params.PCMAudioEncoding = Audio::EPCMAudioEncoding::FLOATING_POINT_32;

    const bool bOpened = MicCapture.OpenAudioCaptureStream(
        Params,
        [this](const void* InAudio, int32 NumFrames, int32 NumChannels, int32 SampleRate, double StreamTime, bool bOverflow)
        {
            (void)StreamTime;
            (void)bOverflow;

            if (!bMicCaptureActive || !InAudio || NumFrames <= 0 || NumChannels <= 0)
            {
                return;
            }

            const float* InFloats = static_cast<const float*>(InAudio);
            const int32 NumSamples = NumFrames * NumChannels;

            FScopeLock Lock(&CaptureMutex);
            if (CapturedSampleRate <= 0)
            {
                CapturedSampleRate = SampleRate;
            }
            if (CapturedNumChannels <= 0)
            {
                CapturedNumChannels = NumChannels;
            }

            const int32 Base = CapturedPcm16.Num();
            CapturedPcm16.AddUninitialized(NumSamples);
            int16* Dest = CapturedPcm16.GetData() + Base;

            for (int32 i = 0; i < NumSamples; ++i)
            {
                const float Clamped = FMath::Clamp(InFloats[i], -1.0f, 1.0f);
                Dest[i] = static_cast<int16>(FMath::RoundToInt(Clamped * 32767.0f));
            }
        },
        1024);

    if (!bOpened)
    {
        const FString Error = TEXT("Failed to open microphone capture stream.");
        UE_LOG(LogLocalTalker, Error, TEXT("[STT] %s"), *Error);
        OnWhisperError.Broadcast(Error);
        return false;
    }

    if (!MicCapture.StartStream())
    {
        MicCapture.CloseStream();
        const FString Error = TEXT("Failed to start microphone capture stream.");
        UE_LOG(LogLocalTalker, Error, TEXT("[STT] %s"), *Error);
        OnWhisperError.Broadcast(Error);
        return false;
    }

    bMicCaptureActive = true;
    UE_LOG(LogLocalTalker, Log, TEXT("[STT] Microphone capture started (deviceIndex=%d, device=\"%s\")."),
        DeviceIndex,
        ResolvedMicName.IsEmpty() ? TEXT("DefaultSystem") : *ResolvedMicName);
    return true;
}

void ULocalPlayerInteractionComponent::CancelMicrophoneCapture()
{
    if (!bMicCaptureActive && !MicCapture.IsCapturing() && !MicCapture.IsStreamOpen())
    {
        return;
    }

    bMicCaptureActive = false;
    if (MicCapture.IsCapturing())
    {
        MicCapture.StopStream();
    }
    if (MicCapture.IsStreamOpen())
    {
        MicCapture.CloseStream();
    }

    FScopeLock Lock(&CaptureMutex);
    const int32 NumSamples = CapturedPcm16.Num();
    const int32 SR = CapturedSampleRate;
    const int32 Ch = CapturedNumChannels;
    CapturedPcm16.Reset();
    CapturedSampleRate = 0;
    CapturedNumChannels = 0;

    const double DurationSec = (SR > 0 && Ch > 0) ? ((double)NumSamples / (double)(SR * Ch)) : 0.0;
    UE_LOG(LogLocalTalker, Log, TEXT("[STT] Microphone capture canceled (samples=%d sr=%d ch=%d dur=%.2fs)."),
        NumSamples, SR, Ch, DurationSec);
}

bool ULocalPlayerInteractionComponent::IsMicrophoneCaptureActive() const
{
    return bMicCaptureActive;
}

bool ULocalPlayerInteractionComponent::StopMicrophoneCaptureAndTranscribe(bool bSendToNearestAI, float MaxRange)
{
    if (!bMicCaptureActive)
    {
        return false;
    }

    bMicCaptureActive = false;

    if (MicCapture.IsCapturing())
    {
        MicCapture.StopStream();
    }
    if (MicCapture.IsStreamOpen())
    {
        MicCapture.CloseStream();
    }

    TArray<int16> Captured;
    int32 SampleRate = 0;
    int32 NumChannels = 0;
    {
        FScopeLock Lock(&CaptureMutex);
        Captured = MoveTemp(CapturedPcm16);
        SampleRate = CapturedSampleRate;
        NumChannels = CapturedNumChannels;
        CapturedSampleRate = 0;
        CapturedNumChannels = 0;
    }

    if (Captured.Num() == 0 || SampleRate <= 0 || NumChannels <= 0)
    {
        const FString Error = TEXT("No microphone audio captured.");
        UE_LOG(LogLocalTalker, Warning, TEXT("[STT] %s"), *Error);
        OnWhisperError.Broadcast(Error);
        return false;
    }

    return QueueTranscriptionFromPcm(MoveTemp(Captured), SampleRate, NumChannels, bSendToNearestAI, MaxRange);
}

bool ULocalPlayerInteractionComponent::TranscribePcm16Buffer(const TArray<int16>& Pcm16Interleaved, int32 SampleRate, int32 NumChannels, bool bSendToNearestAI, float MaxRange)
{
    if (Pcm16Interleaved.Num() <= 0 || SampleRate <= 0 || NumChannels <= 0)
    {
        const FString Error = TEXT("Invalid PCM buffer for Whisper transcription.");
        UE_LOG(LogLocalTalker, Warning, TEXT("[STT] %s"), *Error);
        OnWhisperError.Broadcast(Error);
        return false;
    }

    TArray<int16> Copy = Pcm16Interleaved;
    return QueueTranscriptionFromPcm(MoveTemp(Copy), SampleRate, NumChannels, bSendToNearestAI, MaxRange);
}

void ULocalPlayerInteractionComponent::PrimeWhisperWorkerAsync()
{
    if (GLocalPlayerWhisperPrimed.Load())
    {
        return;
    }

    bool bExpected = false;
    if (!GLocalPlayerWhisperPrimeInFlight.CompareExchange(bExpected, true))
    {
        return;
    }

    TWeakObjectPtr<ULocalPlayerInteractionComponent> WeakThis(this);
    Async(EAsyncExecution::ThreadPool, [WeakThis]()
    {
        FString Error;
        bool bPrimed = false;

        if (ULocalPlayerInteractionComponent* Self = WeakThis.Get())
        {
            bPrimed = Self->PreloadWhisperModelWithWorker(Error);
        }
        else
        {
            Error = TEXT("Whisper prime canceled: component no longer valid.");
        }

        AsyncTask(ENamedThreads::GameThread, [bPrimed, Error]()
        {
            if (bPrimed)
            {
                GLocalPlayerWhisperPrimed.Store(true);
                UE_LOG(LogLocalTalker, Log, TEXT("[STT] Whisper worker primed (model preloaded)."));
            }
            else
            {
                UE_LOG(LogLocalTalker, Warning, TEXT("[STT] Whisper worker prime failed: %s"), *Error);
            }

            GLocalPlayerWhisperPrimeInFlight.Store(false);
        });
    });
}

bool ULocalPlayerInteractionComponent::QueueTranscriptionFromPcm(TArray<int16>&& CapturedInterleavedPcm16, int32 SampleRate, int32 NumChannels, bool bSendToNearestAI, float MaxRange)
{
    if (CapturedInterleavedPcm16.Num() <= 0 || SampleRate <= 0 || NumChannels <= 0)
    {
        const FString Error = TEXT("No valid microphone audio captured.");
        UE_LOG(LogLocalTalker, Warning, TEXT("[STT] %s"), *Error);
        OnWhisperError.Broadcast(Error);
        return false;
    }

    TArray<int16> Mono = DownmixToMono(CapturedInterleavedPcm16, NumChannels);
    if (Mono.Num() == 0)
    {
        const FString Error = TEXT("Captured audio is empty after channel processing.");
        UE_LOG(LogLocalTalker, Warning, TEXT("[STT] %s"), *Error);
        OnWhisperError.Broadcast(Error);
        return false;
    }

    const double CapturedSeconds = (double)CapturedInterleavedPcm16.Num() / (double)(SampleRate * NumChannels);
    const double MonoSeconds = (double)Mono.Num() / (double)SampleRate;
    UE_LOG(LogLocalTalker, Log,
        TEXT("[STT] Transcribe request (rawSamples=%d monoSamples=%d sr=%d ch=%d rawDur=%.2fs monoDur=%.2fs sendToNearest=%d range=%.1f)."),
        CapturedInterleavedPcm16.Num(),
        Mono.Num(),
        SampleRate,
        NumChannels,
        CapturedSeconds,
        MonoSeconds,
        bSendToNearestAI ? 1 : 0,
        MaxRange);

    AddToRoot();
    TWeakObjectPtr<ULocalPlayerInteractionComponent> WeakThis(this);
    Async(EAsyncExecution::ThreadPool,
        [WeakThis, Samples = MoveTemp(Mono), SampleRate, bSendToNearestAI, MaxRange]() mutable
        {
            ULocalPlayerInteractionComponent* Self = WeakThis.Get();
            if (!Self)
            {
                return;
            }

            const FString SttDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("LocalTalker"), TEXT("STT"));
            IFileManager::Get().MakeDirectory(*SttDir, true);
            const FString WavPath = FPaths::Combine(SttDir,
                FString::Printf(TEXT("mic_%s_%s.wav"), *FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S")), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

            FLocalWavPcm16 Wav;
            Wav.SampleRate = SampleRate;
            Wav.NumChannels = 1;
            Wav.Samples = MoveTemp(Samples);

            const double WavDurationSec = (Wav.SampleRate > 0 && Wav.NumChannels > 0)
                ? ((double)Wav.Samples.Num() / (double)(Wav.SampleRate * Wav.NumChannels))
                : 0.0;
            UE_LOG(LogLocalTalker, Log,
                TEXT("[STT] Audio sent to Whisper: path=\"%s\" duration=%.2fs samples=%d sr=%d ch=%d"),
                *WavPath, WavDurationSec, Wav.Samples.Num(), Wav.SampleRate, Wav.NumChannels);

            FString SaveError;
            if (!FLocalTalkerWav::SaveWavPcm16(WavPath, Wav, SaveError))
            {
                AsyncTask(ENamedThreads::GameThread, [WeakThis, SaveError]()
                {
                    if (ULocalPlayerInteractionComponent* Strong = WeakThis.Get())
                    {
                        Strong->OnWhisperError.Broadcast(SaveError);
                        if (Strong->IsRooted())
                        {
                            Strong->RemoveFromRoot();
                        }
                    }
                });
                return;
            }

            FString Text;
            FString TranscribeError;
            const bool bOk = Self->TranscribeWavFileWithWhisper(WavPath, Text, TranscribeError);
            IFileManager::Get().Delete(*WavPath, false, true, true);

            AsyncTask(ENamedThreads::GameThread,
                [WeakThis, bOk, Text, TranscribeError, bSendToNearestAI, MaxRange]()
                {
                    if (ULocalPlayerInteractionComponent* Strong = WeakThis.Get())
                    {
                        if (!bOk)
                        {
                            Strong->OnWhisperError.Broadcast(TranscribeError);
                            if (Strong->IsRooted())
                            {
                                Strong->RemoveFromRoot();
                            }
                            return;
                        }

                        const FString Trimmed = Text.TrimStartAndEnd();
                        if (Trimmed.IsEmpty())
                        {
                            UE_LOG(LogLocalTalker, Log, TEXT("[STT] Whisper transcript empty; treating as silence."));
                            Strong->OnWhisperTranscription.Broadcast(FString());
                            if (Strong->IsRooted())
                            {
                                Strong->RemoveFromRoot();
                            }
                            return;
                        }

                        UE_LOG(LogLocalTalker, Log,
                            TEXT("[STT] Whisper transcript (%d chars): \"%s\""),
                            Trimmed.Len(), *Trimmed);

                        Strong->OnWhisperTranscription.Broadcast(Trimmed);
                        if (bSendToNearestAI && !Strong->SpeakToNearestAI(Trimmed, MaxRange))
                        {
                            Strong->OnWhisperError.Broadcast(TEXT("Transcription succeeded but no nearby LocalTalk AI accepted the message."));
                        }
                        if (Strong->IsRooted())
                        {
                            Strong->RemoveFromRoot();
                        }
                    }
                });
        });

    return true;
}

bool ULocalPlayerInteractionComponent::PreloadWhisperModelWithWorker(FString& OutError)
{
    OutError.Reset();

    FScopeLock WorkerLock(&GLocalPlayerWhisperWorkerMutex);
    if (!EnsureWhisperWorker(OutError))
    {
        return false;
    }

    const ULocalTalkerSettings* Settings = GetDefault<ULocalTalkerSettings>();
    const FLocalTalkerRuntimePaths Paths = ResolveRuntimePaths();
    const FString RequestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Model = Settings ? Settings->WhisperModel : TEXT("base.en");
    const double TimeoutSeconds = static_cast<double>(Settings ? FMath::Clamp(Settings->WhisperRequestTimeoutSeconds, 5.0f, 30.0f) : 20.0f);

    TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
    Request->SetStringField(TEXT("cmd"), TEXT("preload"));
    Request->SetStringField(TEXT("id"), RequestId);
    Request->SetStringField(TEXT("model"), Model);
    if (!Paths.WhisperCacheDir.IsEmpty())
    {
        Request->SetStringField(TEXT("cache_dir"), Paths.WhisperCacheDir);
    }

    FString RequestLine;
    if (!SerializeJsonLine(Request, RequestLine))
    {
        OutError = TEXT("Failed to serialize Whisper preload request JSON.");
        return false;
    }

    if (!FLocalTalkerProcess::WriteStdin(GLocalPlayerWhisperWorker->Pipes, RequestLine))
    {
        OutError = TEXT("Failed to send Whisper preload request.");
        return false;
    }

    const double StartSeconds = FPlatformTime::Seconds();
    while ((FPlatformTime::Seconds() - StartSeconds) < TimeoutSeconds)
    {
        if (!GLocalPlayerWhisperWorker->Handle.IsValid() || !FPlatformProcess::IsProcRunning(GLocalPlayerWhisperWorker->Handle))
        {
            OutError = TEXT("Whisper worker terminated during preload.");
            return false;
        }

        GLocalPlayerWhisperWorker->StdoutBuffer += FLocalTalkerProcess::ReadAvailable(GLocalPlayerWhisperWorker->Pipes.ReadPipe);
        GLocalPlayerWhisperWorker->StderrBuffer += FLocalTalkerProcess::ReadAvailable(GLocalPlayerWhisperWorker->Pipes.ReadErrPipe);

        FString Line;
        while (TryPopLine(GLocalPlayerWhisperWorker->StderrBuffer, Line))
        {
            UE_LOG(LogLocalTalker, Verbose, TEXT("[STT][whisper stderr] %s"), *Line);
        }

        while (TryPopLine(GLocalPlayerWhisperWorker->StdoutBuffer, Line))
        {
            TSharedPtr<FJsonObject> Json;
            if (!ParseJsonLine(Line, Json) || !Json.IsValid())
            {
                UE_LOG(LogLocalTalker, Verbose, TEXT("[STT][whisper stdout] %s"), *Line);
                continue;
            }

            FString ResponseId;
            if (!Json->TryGetStringField(TEXT("id"), ResponseId) || ResponseId != RequestId)
            {
                continue;
            }

            bool bOk = false;
            Json->TryGetBoolField(TEXT("ok"), bOk);
            if (!bOk)
            {
                if (!Json->TryGetStringField(TEXT("error"), OutError) || OutError.IsEmpty())
                {
                    OutError = TEXT("Whisper preload failed with unknown worker error.");
                }
                return false;
            }

            return true;
        }

        FPlatformProcess::Sleep(0.01f);
    }

    OutError = FString::Printf(TEXT("Whisper preload timed out after %.1f seconds."), TimeoutSeconds);
    return false;
}

bool ULocalPlayerInteractionComponent::EnsureWhisperWorker(FString& OutError)
{
    OutError.Reset();

    if (!GLocalPlayerWhisperWorker.IsValid())
    {
        GLocalPlayerWhisperWorker = MakeUnique<FLocalPlayerWhisperWorker>();
    }

    if (GLocalPlayerWhisperWorker->Handle.IsValid() && FPlatformProcess::IsProcRunning(GLocalPlayerWhisperWorker->Handle))
    {
        return true;
    }

    if (GLocalPlayerWhisperWorker->Handle.IsValid())
    {
        FPlatformProcess::TerminateProc(GLocalPlayerWhisperWorker->Handle, true);
        FPlatformProcess::CloseProc(GLocalPlayerWhisperWorker->Handle);
        GLocalPlayerWhisperWorker->Handle.Reset();
        GLocalPlayerWhisperPrimed.Store(false);
    }
    FLocalTalkerProcess::ClosePipes(GLocalPlayerWhisperWorker->Pipes);
    GLocalPlayerWhisperWorker->StdoutBuffer.Reset();
    GLocalPlayerWhisperWorker->StderrBuffer.Reset();

    const FLocalTalkerRuntimePaths Paths = ResolveRuntimePaths();
    if (Paths.WhisperWorkerScriptPath.IsEmpty())
    {
        OutError = TEXT("Whisper worker script path is empty.");
        return false;
    }

    FString ScriptPath = Paths.WhisperWorkerScriptPath;
    if (FPaths::IsRelative(ScriptPath))
    {
        ScriptPath = FPaths::ConvertRelativePathToFull(ScriptPath);
    }

    if (!FPaths::FileExists(ScriptPath))
    {
        OutError = FString::Printf(TEXT("Whisper worker script not found: %s"), *ScriptPath);
        return false;
    }

    const FString PythonExe = Paths.WhisperPythonExePath.IsEmpty() ? TEXT("python") : Paths.WhisperPythonExePath;
    const FString Args = QuoteArg(ScriptPath);
    const FString WorkingDir = Paths.WorkingDir;

    FString SpawnError;
    if (!FLocalTalkerProcess::SpawnWithPipes(PythonExe, Args, WorkingDir, GLocalPlayerWhisperWorker->Handle, GLocalPlayerWhisperWorker->Pipes, SpawnError))
    {
        OutError = FString::Printf(TEXT("Failed to start Whisper worker: %s"), *SpawnError);
        return false;
    }

    UE_LOG(LogLocalTalker, Log, TEXT("[STT] Whisper worker started: %s %s"), *PythonExe, *Args);
    return true;
}

bool ULocalPlayerInteractionComponent::TranscribeWavFileWithWhisper(const FString& WavPath, FString& OutText, FString& OutError)
{
    OutText.Reset();
    OutError.Reset();

    FScopeLock WorkerLock(&GLocalPlayerWhisperWorkerMutex);

    if (!EnsureWhisperWorker(OutError))
    {
        return false;
    }

    const ULocalTalkerSettings* Settings = GetDefault<ULocalTalkerSettings>();
    const FLocalTalkerRuntimePaths Paths = ResolveRuntimePaths();

    const FString RequestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Model = Settings ? Settings->WhisperModel : TEXT("base.en");
    const FString Language = Settings ? Settings->WhisperLanguage : TEXT("en");
    const bool bVadFilter = Settings ? Settings->bWhisperVadFilter : true;
    const double TimeoutSeconds = static_cast<double>(Settings ? FMath::Max(5.0f, Settings->WhisperRequestTimeoutSeconds) : 90.0f);
    UE_LOG(LogLocalTalker, Log,
        TEXT("[STT] Whisper request id=%s model=%s language=\"%s\" vad=%d timeout=%.1fs wav=%s"),
        *RequestId,
        *Model,
        Language.IsEmpty() ? TEXT("<auto>") : *Language,
        bVadFilter ? 1 : 0,
        TimeoutSeconds,
        *WavPath);

    TSharedRef<FJsonObject> Request = MakeShared<FJsonObject>();
    Request->SetStringField(TEXT("cmd"), TEXT("transcribe"));
    Request->SetStringField(TEXT("id"), RequestId);
    Request->SetStringField(TEXT("audio_path"), WavPath);
    Request->SetStringField(TEXT("model"), Model);
    if (!Language.TrimStartAndEnd().IsEmpty())
    {
        Request->SetStringField(TEXT("language"), Language);
    }
    Request->SetBoolField(TEXT("vad_filter"), bVadFilter);
    if (!Paths.WhisperCacheDir.IsEmpty())
    {
        Request->SetStringField(TEXT("cache_dir"), Paths.WhisperCacheDir);
    }

    FString RequestLine;
    if (!SerializeJsonLine(Request, RequestLine))
    {
        OutError = TEXT("Failed to serialize Whisper request JSON.");
        return false;
    }

    if (!FLocalTalkerProcess::WriteStdin(GLocalPlayerWhisperWorker->Pipes, RequestLine))
    {
        OutError = TEXT("Failed to send request to Whisper worker stdin.");
        return false;
    }

    const double StartSeconds = FPlatformTime::Seconds();
    while ((FPlatformTime::Seconds() - StartSeconds) < TimeoutSeconds)
    {
        if (!GLocalPlayerWhisperWorker->Handle.IsValid() || !FPlatformProcess::IsProcRunning(GLocalPlayerWhisperWorker->Handle))
        {
            OutError = TEXT("Whisper worker terminated unexpectedly.");
            return false;
        }

        GLocalPlayerWhisperWorker->StdoutBuffer += FLocalTalkerProcess::ReadAvailable(GLocalPlayerWhisperWorker->Pipes.ReadPipe);
        GLocalPlayerWhisperWorker->StderrBuffer += FLocalTalkerProcess::ReadAvailable(GLocalPlayerWhisperWorker->Pipes.ReadErrPipe);

        FString Line;
        while (TryPopLine(GLocalPlayerWhisperWorker->StderrBuffer, Line))
        {
            UE_LOG(LogLocalTalker, Verbose, TEXT("[STT][whisper stderr] %s"), *Line);
        }

        while (TryPopLine(GLocalPlayerWhisperWorker->StdoutBuffer, Line))
        {
            TSharedPtr<FJsonObject> Json;
            if (!ParseJsonLine(Line, Json) || !Json.IsValid())
            {
                UE_LOG(LogLocalTalker, Verbose, TEXT("[STT][whisper stdout] %s"), *Line);
                continue;
            }

            FString ResponseId;
            if (!Json->TryGetStringField(TEXT("id"), ResponseId) || ResponseId != RequestId)
            {
                continue;
            }

            bool bOk = false;
            Json->TryGetBoolField(TEXT("ok"), bOk);
            if (!bOk)
            {
                if (!Json->TryGetStringField(TEXT("error"), OutError) || OutError.IsEmpty())
                {
                    OutError = TEXT("Whisper worker returned an unknown error.");
                }
                return false;
            }

            if (!Json->TryGetStringField(TEXT("text"), OutText))
            {
                OutText.Reset();
            }
            const double Took = FPlatformTime::Seconds() - StartSeconds;
            UE_LOG(LogLocalTalker, Log, TEXT("[STT] Whisper response id=%s ok=1 chars=%d took=%.2fs"),
                *RequestId,
                OutText.Len(),
                Took);
            return true;
        }

        FPlatformProcess::Sleep(0.01f);
    }

    OutError = FString::Printf(TEXT("Whisper transcription timed out after %.1f seconds."), TimeoutSeconds);
    UE_LOG(LogLocalTalker, Warning, TEXT("[STT] Whisper request id=%s timed out after %.1fs"), *RequestId, TimeoutSeconds);
    return false;
}

void ULocalPlayerInteractionComponent::ShutdownWhisperWorker()
{
    if (!GLocalPlayerWhisperWorkerMutex.TryLock())
    {
        return;
    }
    ON_SCOPE_EXIT
    {
        GLocalPlayerWhisperWorkerMutex.Unlock();
    };

    if (!GLocalPlayerWhisperWorker.IsValid())
    {
        return;
    }

    if (GLocalPlayerWhisperWorker->Handle.IsValid() && FPlatformProcess::IsProcRunning(GLocalPlayerWhisperWorker->Handle))
    {
        TSharedRef<FJsonObject> Shutdown = MakeShared<FJsonObject>();
        Shutdown->SetStringField(TEXT("cmd"), TEXT("shutdown"));

        FString ShutdownLine;
        if (SerializeJsonLine(Shutdown, ShutdownLine))
        {
            FLocalTalkerProcess::WriteStdin(GLocalPlayerWhisperWorker->Pipes, ShutdownLine);
        }

        const double Start = FPlatformTime::Seconds();
        while (FPlatformProcess::IsProcRunning(GLocalPlayerWhisperWorker->Handle) && (FPlatformTime::Seconds() - Start) < 1.0)
        {
            FLocalTalkerProcess::ReadAvailable(GLocalPlayerWhisperWorker->Pipes.ReadPipe);
            FLocalTalkerProcess::ReadAvailable(GLocalPlayerWhisperWorker->Pipes.ReadErrPipe);
            FPlatformProcess::Sleep(0.01f);
        }

        if (FPlatformProcess::IsProcRunning(GLocalPlayerWhisperWorker->Handle))
        {
            FPlatformProcess::TerminateProc(GLocalPlayerWhisperWorker->Handle, true);
        }
        FPlatformProcess::CloseProc(GLocalPlayerWhisperWorker->Handle);
    }

    FLocalTalkerProcess::ClosePipes(GLocalPlayerWhisperWorker->Pipes);
    GLocalPlayerWhisperWorker.Reset();
    GLocalPlayerWhisperPrimed.Store(false);
    GLocalPlayerWhisperPrimeInFlight.Store(false);
}
