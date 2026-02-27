#include "LocalCharacterComponent.h"
#include "LocalTalkerSettings.h"
#include "LocalTalkerInProcAsync.h"
#include "LocalTalkerProcess.h"
#include "LocalTalkerWav.h"
#include "LocalTalkerLog.h"
#include "LocalTalkConversationSubsystem.h"

#include "AudioCaptureCore.h"
#include "Async/Async.h"
#include "Engine/Engine.h"
#include "SubtitleManager.h"
#include "Misc/FileHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Misc/PathViews.h"
#include "Misc/ScopeLock.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"
#include "Misc/Base64.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundWave.h"

static FString LocalTalkerTimePrefix(const UObject* Obj)
{
    const UWorld* W = Obj ? Obj->GetWorld() : nullptr;
    if (!W)
    {
        return TEXT("");
    }
    return FString::Printf(TEXT("[t=%.2f] "), W->GetTimeSeconds());
}

static TAutoConsoleVariable<int32> CVarLocalTalkerLogAudioTrace(
    TEXT("LocalTalker.LogAudioTrace"),
    1,
    TEXT("Detailed LocalTalker audio/TTS diagnostics.\n")
    TEXT("0: Off\n")
    TEXT("1: On"),
    ECVF_Default);

static bool LocalTalkerShouldLogAudioTrace()
{
    return CVarLocalTalkerLogAudioTrace.GetValueOnAnyThread() != 0;
}

static FString LocalTalkerPreview(const FString& In, int32 MaxLen = 96)
{
    FString Out = In;
    Out.ReplaceInline(TEXT("\n"), TEXT(" "));
    Out.ReplaceInline(TEXT("\r"), TEXT(" "));
    Out.TrimStartAndEndInline();
    if (Out.Len() > MaxLen)
    {
        Out = Out.Left(MaxLen) + TEXT("...");
    }
    return Out;
}


static FString QuoteArg3(const FString& S)
{
    FString T = S;
    T.ReplaceInline(TEXT("\""), TEXT("\\\""));
    return FString::Printf(TEXT("\"%s\""), *T);
}

struct FLocalTtsWorkerState
{
    FProcHandle Handle;
    FLocalProcPipes Pipes;
    FString StdoutBuffer;
    FString StderrBuffer;
};

struct FLocalTtsWorkerPoolEntry
{
    FLocalTtsWorkerState Worker;
    FCriticalSection RequestMutex;
    FThreadSafeCounter InFlightRequests;
};

struct FLocalTalkerSharedTtsWorkerState
{
    FString WorkerKey;
    TArray<TUniquePtr<FLocalTtsWorkerPoolEntry>> Workers;
};

// --- Kokoro ONNX shared worker pool ---
static TUniquePtr<FLocalTalkerSharedTtsWorkerState> GLocalTalkerSharedKokoroWorker;
static FCriticalSection GLocalTalkerSharedKokoroWorkerMutex;

static void LocalTalkerShutdownWorkerStateNoLock(FLocalTtsWorkerState& Worker)
{
    if (Worker.Handle.IsValid())
    {
        if (Worker.Pipes.WriteInPipe)
        {
            FLocalTalkerProcess::WriteStdin(Worker.Pipes, TEXT("{\"cmd\":\"shutdown\"}\n"));
        }

        const double Start = FPlatformTime::Seconds();
        while (FPlatformProcess::IsProcRunning(Worker.Handle) && (FPlatformTime::Seconds() - Start) < 1.0)
        {
            FLocalTalkerProcess::ReadAvailable(Worker.Pipes.ReadPipe);
            FLocalTalkerProcess::ReadAvailable(Worker.Pipes.ReadErrPipe);
            FPlatformProcess::Sleep(0.01f);
        }

        if (FPlatformProcess::IsProcRunning(Worker.Handle))
        {
            FPlatformProcess::TerminateProc(Worker.Handle, true);
        }
        FPlatformProcess::CloseProc(Worker.Handle);
    }

    FLocalTalkerProcess::ClosePipes(Worker.Pipes);
    Worker.StdoutBuffer.Reset();
    Worker.StderrBuffer.Reset();
}

static void LocalTalkerShutdownSharedTtsWorkerNoLock(TUniquePtr<FLocalTalkerSharedTtsWorkerState>& SharedWorker)
{
    if (!SharedWorker.IsValid())
    {
        return;
    }

    const double WaitStart = FPlatformTime::Seconds();
    while (true)
    {
        int32 TotalInFlight = 0;
        for (const TUniquePtr<FLocalTtsWorkerPoolEntry>& Entry : SharedWorker->Workers)
        {
            if (Entry.IsValid())
            {
                TotalInFlight += Entry->InFlightRequests.GetValue();
            }
        }

        if (TotalInFlight <= 0 || (FPlatformTime::Seconds() - WaitStart) > 5.0)
        {
            break;
        }
        FPlatformProcess::Sleep(0.01f);
    }

    for (TUniquePtr<FLocalTtsWorkerPoolEntry>& Entry : SharedWorker->Workers)
    {
        if (Entry.IsValid())
        {
            LocalTalkerShutdownWorkerStateNoLock(Entry->Worker);
        }
    }
    SharedWorker->Workers.Reset();
    SharedWorker.Reset();
}

static FString LocalTalkerBuildKokoroWorkerKey(
    const FString& PythonExe,
    const FString& ScriptPath,
    const FString& CacheDir,
    float Speed,
    int32 PoolSize)
{
    return FString::Printf(TEXT("kokoro|%s|%s|%s|speed=%.2f|pool=%d"),
        *PythonExe, *ScriptPath, *CacheDir, Speed, PoolSize);
}

static bool LocalTalkerTryPopLine(FString& InOutBuffer, FString& OutLine)
{
    int32 NewlineIdx = INDEX_NONE;
    if (!InOutBuffer.FindChar(TEXT('\n'), NewlineIdx))
    {
        return false;
    }

    OutLine = InOutBuffer.Left(NewlineIdx);
    InOutBuffer = InOutBuffer.Mid(NewlineIdx + 1);
    OutLine.ReplaceInline(TEXT("\r"), TEXT(""));
    OutLine.TrimStartAndEndInline();
    return true;
}

static bool LocalTalkerSerializeJsonLine(const TSharedRef<FJsonObject>& Obj, FString& OutLine)
{
    OutLine.Reset();
    // Worker protocol is one-JSON-object-per-line; pretty-printing introduces newlines that break parsing.
    TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutLine);
    if (!FJsonSerializer::Serialize(Obj, Writer))
    {
        return false;
    }
    OutLine += TEXT("\n");
    return true;
}

static bool LocalTalkerParseJsonLine(const FString& Line, TSharedPtr<FJsonObject>& OutObj)
{
    OutObj.Reset();
    if (Line.IsEmpty())
    {
        return false;
    }

    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
    return FJsonSerializer::Deserialize(Reader, OutObj) && OutObj.IsValid();
}

static ELocalTalkMicInputDeviceMode LocalTalkerResolveMicDeviceMode(
    const ULocalCharacterComponent* Component,
    FString& OutNamedDevice)
{
    OutNamedDevice.Reset();
    if (!Component)
    {
        return ELocalTalkMicInputDeviceMode::DefaultSystem;
    }

    if (Component->bUseProjectSettingsMicInput)
    {
        if (const ULocalTalkerSettings* S = GetDefault<ULocalTalkerSettings>())
        {
            OutNamedDevice = S->MicInputDeviceName;
            return S->MicInputDeviceMode;
        }
    }

    OutNamedDevice = Component->MicInputDeviceName;
    return Component->MicInputDeviceMode;
}

static bool IsSentenceTerminator(TCHAR C)
{
    // Avoid treating '\n' as a terminator: it causes JSON-like outputs to get split into stray `"}"` chunks.
    return C == TEXT('.') || C == TEXT('!') || C == TEXT('?');
}

static bool IsEllipsisAt(const FString& S, int32 Index)
{
    if (Index < 0 || Index >= S.Len()) return false;
    if (S[Index] != TEXT('.')) return false;
    const bool bPrevDot = (Index > 0 && S[Index - 1] == TEXT('.'));
    const bool bNextDot = (Index + 1 < S.Len() && S[Index + 1] == TEXT('.'));
    return bPrevDot || bNextDot;
}

static bool LocalTalkerHasAlphaNum(const FString& S)
{
    for (int32 i = 0; i < S.Len(); i++)
    {
        if (FChar::IsAlnum(S[i]))
        {
            return true;
        }
    }
    return false;
}


static FString TrimSentence(const FString& In)
{
    FString S = In;
    S.ReplaceInline(TEXT("\r"), TEXT(""));
    S.TrimStartAndEndInline();
    return S;
}

static void LocalTalkerDumpPromptToFile(const FString& Speaker, const FString& Prompt)
{
    const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("LocalTalker"), TEXT("LLMLogs"));
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*Dir);

    const FString SafeSpeaker = Speaker.IsEmpty() ? TEXT("Speaker") : Speaker;
    const FString FileName = FString::Printf(TEXT("%s_%s_prompt.txt"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S_%s")), *SafeSpeaker);
    const FString Path = FPaths::Combine(Dir, FileName);

    FFileHelper::SaveStringToFile(Prompt, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
ULocalCharacterComponent::ULocalCharacterComponent()
{
    PrimaryComponentTick.bCanEverTick = true;

    // Sensible prompt defaults (editable on the component).
    Directions =
        TEXT("You are a helpful game character in Unreal Engine.\n")
        TEXT("Follow the player's instructions carefully.\n")
        TEXT("If you are unsure, ask a short clarifying question.\n")
        TEXT("Keep responses concise and actionable.\n")
        TEXT("Do not mention being an AI or a language model.\n");

    Desc = TEXT("");

    // Prefer real UE subtitles instead of debug prints.
    bUseUESubtitles = true;
    bShowOnScreenSubtitles = false;
}

ULocalCharacterComponent::~ULocalCharacterComponent()
{
}

void ULocalCharacterComponent::BeginPlay()
{
    Super::BeginPlay();
    EnsureAudio();

    if (UWorld* W = GetWorld())
    {
        if (auto* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            Sub->RegisterTalker(this);
        }
    }

    // Quick visibility into what the component is using at runtime.
    const FLocalTalkerRuntimePaths Paths = ResolvePaths();
    UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] LocalTalker paths: LlamaLib='%s' LlamaModel='%s' KokoroPy='%s' KokoroWorker='%s' WorkDir='%s'"),
        *LocalTalkerTimePrefix(this),
        *GetSpeakerNameResolved(),
        *Paths.LlamaLibPath,
        *Paths.LlamaModelPath,
        *Paths.KokoroPythonExePath,
        *Paths.KokoroWorkerScriptPath,
        *Paths.WorkingDir
    );

    // Mic diagnostics: show mode, desired name, resolved device, and all available capture devices.
    FString DesiredMicName;
    const ELocalTalkMicInputDeviceMode MicMode = LocalTalkerResolveMicDeviceMode(this, DesiredMicName);
    const UEnum* MicEnum = StaticEnum<ELocalTalkMicInputDeviceMode>();
    const FString MicModeStr = MicEnum
        ? MicEnum->GetNameStringByValue(static_cast<int64>(MicMode))
        : TEXT("<Unknown>");

    const FString ResolvedMicDevice = GetResolvedMicInputDeviceName();
    const bool bUsingDefaultSystem = ResolvedMicDevice.IsEmpty();

    const TArray<FString> AllDevices = GetMicInputDeviceOptions();
    const FString AllDevicesJoined = AllDevices.Num() > 0
        ? FString::Join(AllDevices, TEXT(", "))
        : TEXT("<None>");

    UE_LOG(LogLocalTalker, Log,
        TEXT("%s[%s] Mic input config: Mode=%s DesiredName=\"%s\" Using=\"%s\" Available=[%s]"),
        *LocalTalkerTimePrefix(this),
        *GetSpeakerNameResolved(),
        *MicModeStr,
        DesiredMicName.IsEmpty() ? TEXT("<Default>") : *DesiredMicName,
        bUsingDefaultSystem ? TEXT("Default (System)") : *ResolvedMicDevice,
        *AllDevicesJoined);

}

void ULocalCharacterComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Interrupt();
    StopTTSWorker();

    if (UWorld* W = GetWorld())
    {
        if (auto* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            Sub->UnregisterTalker(this);
        }
    }

    Super::EndPlay(EndPlayReason);
}

void ULocalCharacterComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (bInterrupted) return;

    const double Now = FPlatformTime::Seconds();
    const bool bTimeout = (LLMTextBuffer.Len() > 0) && ((Now - LastTextAppendSeconds) >= FlushSeconds);

    ExtractAndEnqueueSentences(bTimeout);

    if (bLLMFinished && LLMTextBuffer.Len() > 0)
    {
        ExtractAndEnqueueSentences(true);
    }

    if (AudioComp && !bAudioPlaybackComplete)
    {
        if (UWorld* W = GetWorld())
        {
            const double NowSeconds = W->GetTimeSeconds();

            if (!AudioComp->IsPlaying() && !PendingAudioWave)
            {
                // Guard: IsPlaying() can be false for 1-2 frames right after AudioComp->Play()
                // while the audio thread processes the command.  Don't call HandleAudioFinished
                // during this transient window or the audio is silently dropped.
                const bool bRecentStart = (ActiveAudioStartWorldSeconds > 0.0) &&
                    ((NowSeconds - ActiveAudioStartWorldSeconds) < 0.5);
                if (!bRecentStart)
                {
                    HandleAudioFinished();
                }
            }
            else if (AudioComp->IsPlaying() && ActiveAudioStartWorldSeconds > 0.0 && ActiveAudioDurationSeconds > 0.0f)
            {
                const double EndSeconds = ActiveAudioStartWorldSeconds + (double)ActiveAudioDurationSeconds + (double)FMath::Max(0.0f, AudioCompletionGraceSeconds);
                if (NowSeconds >= EndSeconds)
                {
                    AudioComp->Stop();
                    HandleAudioFinished();
                }
            }
        }
        else
        {
            // No world — simple fallback (no transient guard needed, no world time available).
            if (!AudioComp->IsPlaying() && !PendingAudioWave)
            {
                HandleAudioFinished();
            }
        }
    }

    // Notify the Director when this turn has finished LLM/TTS.
    // Audio playback may still be in progress; this allows the next turn to prewarm while we finish playing.
    if (!bNotifiedSubsystemFinished &&
        bLLMFinished &&
        PendingSentenceCount.GetValue() == 0)
    {
        bNotifiedSubsystemFinished = true;
        if (UWorld* W = GetWorld())
        {
            if (auto* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
            {
                Sub->ReleaseTurn(this);
            }
        }
    }
}

void ULocalCharacterComponent::EnsureAudio()
{
    AActor* Owner = GetOwner();
    if (!Owner) return;
    const bool bTraceAudio = LocalTalkerShouldLogAudioTrace();
    const bool bWasNull = (AudioComp == nullptr);

    if (!AudioComp)
    {
        AudioComp = Owner->FindComponentByClass<UAudioComponent>();
        if (!AudioComp)
        {
            AudioComp = NewObject<UAudioComponent>(Owner, TEXT("LocalTalkerAudio"));
            AudioComp->bAutoActivate = false;
            AudioComp->RegisterComponent();
            if (Owner->GetRootComponent())
            {
                AudioComp->AttachToComponent(Owner->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
            }
            if (bTraceAudio)
            {
                UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] EnsureAudio: created UAudioComponent."),
                    *LocalTalkerTimePrefix(this),
                    *GetSpeakerNameResolved());
            }
        }
        else if (bTraceAudio)
        {
            UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] EnsureAudio: using existing owner UAudioComponent."),
                *LocalTalkerTimePrefix(this),
                *GetSpeakerNameResolved());
        }
    }

    AudioComp->SetVolumeMultiplier(FMath::Max(0.0f, VoiceVolumeMultiplier));

    if (bUseLocalSound)
    {
        AudioComp->bAllowSpatialization = true;
        AudioComp->bIsUISound = false;
        AudioComp->bOverrideAttenuation = true;

        const float Radius = FMath::Max(0.0f, GetHearingRadius());
        FSoundAttenuationSettings& Attn = AudioComp->AttenuationOverrides;
        Attn.bAttenuate = true;
        Attn.bSpatialize = true;
        Attn.AttenuationShape = EAttenuationShape::Sphere;
        Attn.AttenuationShapeExtents = FVector(Radius, 0.0f, 0.0f);
        Attn.FalloffDistance = FMath::Max(0.0f, Radius * 0.2f);
    }
    else
    {
        AudioComp->bAllowSpatialization = false;
        AudioComp->bIsUISound = true;
        AudioComp->bOverrideAttenuation = false;
    }

    AudioComp->OnAudioFinished.AddUniqueDynamic(this, &ULocalCharacterComponent::HandleAudioFinished);
    if (bTraceAudio && bWasNull)
    {
        UE_LOG(LogLocalTalker, Log,
            TEXT("%s[%s] EnsureAudio configured: comp=%p local=%d spatial=%d ui=%d vol=%.2f hearingRadius=%.1f playing=%d"),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved(),
            AudioComp,
            bUseLocalSound ? 1 : 0,
            AudioComp->bAllowSpatialization ? 1 : 0,
            AudioComp->bIsUISound ? 1 : 0,
            AudioComp->VolumeMultiplier,
            GetHearingRadius(),
            AudioComp->IsPlaying() ? 1 : 0);
    }
}

FString ULocalCharacterComponent::GetSpeakerNameResolved() const
{
    if (!SpeakerName.IsEmpty()) return SpeakerName;
    if (const AActor* Owner = GetOwner())
    {
        return Owner->GetName();
    }
    return TEXT("LocalTalker");
}

TArray<FString> ULocalCharacterComponent::GetVoiceOptions() const
{
    TArray<FString> Out;

    // 1) Explicit list from settings
    if (const ULocalTalkerSettings* S = GetDefault<ULocalTalkerSettings>())
    {
        for (const FLocalTalkVoiceOption& V : S->Voices)
        {
            if (V.Id.IsNone()) continue;
            if (!V.KokoroVoice.IsEmpty())
            {
                Out.Add(V.Id.ToString());
            }
        }
    }

    // 2) Auto-discover any `.voiceprompt.pt` files in plugin voices folder.
    // These are externally-generated prompt assets for Base model inference.
    if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LocalTalker")))
    {
        const FString VoicesDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/Voices"));
        TArray<FString> Found;
        IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
        PF.FindFiles(Found, *VoicesDir, TEXT(".voiceprompt.pt"));
        for (const FString& FoundPath : Found)
        {
            FString FullPath = FoundPath;
            if (FPaths::IsRelative(FullPath))
            {
                FullPath = FPaths::Combine(VoicesDir, FullPath);
            }
            if (!FPaths::FileExists(FullPath)) continue;

            const FString Base = FString(FPathViews::GetCleanFilename(FullPath));
            FString Stem = Base;
            Stem.RemoveFromEnd(TEXT(".voiceprompt.pt"));
            Out.AddUnique(Stem);
        }
    }

    Out.Sort();
    return Out;
}

TArray<FString> ULocalCharacterComponent::GetMicInputDeviceOptions() const
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

FString ULocalCharacterComponent::GetResolvedMicInputDeviceName() const
{
    FString DesiredName;
    const ELocalTalkMicInputDeviceMode Mode = LocalTalkerResolveMicDeviceMode(this, DesiredName);
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

    // If requested device no longer exists, fall back to default.
    return FString();
}

void ULocalCharacterComponent::DebugPrintLine(const FString& Line, float Seconds, bool bNewLine) const
{
    if (!GEngine) return;
    // Stable per-component line (plus optional offset when we want a second line).
    const int32 KeyBase = (int32)((PTRINT)this & 0x7fffffff);
    const int32 Key = bNewLine ? (KeyBase + 1) : KeyBase;
    GEngine->AddOnScreenDebugMessage(Key, Seconds, FColor::Cyan, Line);
}

void ULocalCharacterComponent::EmitSubtitle(const FString& Text, float DurationSeconds)
{
    const FString Speaker = GetSpeakerNameResolved();
    OnSubtitle.Broadcast(Speaker, Text);
    OnSubtitleNative.Broadcast(Speaker, Text);

    if (bShowOnScreenSubtitles)
    {
        const float Seconds = (DurationSeconds > 0.0f) ? DurationSeconds : OnScreenSubtitleSeconds;
        DebugPrintLine(FString::Printf(TEXT("%s: %s"), *Speaker, *Text), Seconds, /*bNewLine*/ true);
    }
}

static FString TrimToLastNChars(const FString& In, int32 MaxChars)
{
    if (MaxChars <= 0) return FString();
    if (In.Len() <= MaxChars) return In;
    return In.Right(MaxChars);
}

static bool LocalTalkerModelLooksLikeLlama3(const FString& ModelPath)
{
    const FString Base = FPaths::GetBaseFilename(ModelPath).ToLower();
    return Base.Contains(TEXT("llama-3")) || Base.Contains(TEXT("llama3"));
}

static FString LocalTalkerOneLine(const FString& In)
{
    FString S = In;
    S.ReplaceInline(TEXT("\r"), TEXT(" "));
    S.ReplaceInline(TEXT("\n"), TEXT(" "));
    S.ReplaceInline(TEXT("\t"), TEXT(" "));
    S.TrimStartAndEndInline();
    while (S.Contains(TEXT("  ")))
    {
        S.ReplaceInline(TEXT("  "), TEXT(" "));
    }
    return S;
}

static FString LocalTalkerJsonEscape(const FString& In)
{
    FString S = In;
    S.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
    S.ReplaceInline(TEXT("\""), TEXT("\\\""));
    S.ReplaceInline(TEXT("\r"), TEXT("\\r"));
    S.ReplaceInline(TEXT("\n"), TEXT("\\n"));
    S.ReplaceInline(TEXT("\t"), TEXT("\\t"));
    return S;
}

static FString LocalTalkerTagForName(const FString& InName)
{
    FString Name = InName;
    Name.TrimStartAndEndInline();
    if (Name.IsEmpty()) return TEXT("SPEAKER");

    FString Out;
    Out.Reserve(Name.Len());
    bool bPrevUnderscore = false;

    for (int32 i = 0; i < Name.Len(); i++)
    {
        const TCHAR C = Name[i];
        if (FChar::IsAlnum(C))
        {
            Out.AppendChar(FChar::ToUpper(C));
            bPrevUnderscore = false;
        }
        else if (!bPrevUnderscore)
        {
            Out.AppendChar(TEXT('_'));
            bPrevUnderscore = true;
        }
    }

    Out.TrimStartAndEndInline();
    while (Out.StartsWith(TEXT("_"))) Out = Out.Mid(1);
    while (Out.EndsWith(TEXT("_"))) Out.LeftChopInline(1);

    return Out.IsEmpty() ? TEXT("SPEAKER") : Out;
}

static FString LocalTalkerTagForSpeakerName(const FString& Name, bool bFromUser)
{
    if (bFromUser) return TEXT("PLAYER");
    if (Name.IsEmpty()) return TEXT("SPEAKER");
    if (Name.Equals(TEXT("User"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("Player"), ESearchCase::IgnoreCase))
    {
        return TEXT("PLAYER");
    }
    return LocalTalkerTagForName(Name);
}

static void LocalTalkerStripControlTokens(FString& S)
{
    static const TCHAR* Tokens[] =
    {
        TEXT("<|assistant|>"),
        TEXT("<|user|>"),
        TEXT("<|system|>"),
        TEXT("<|eot_id|>"),
        TEXT("<|begin_of_text|>"),
        TEXT("<|end_of_text|>"),
        TEXT("<|start_header_id|>assistant<|end_header_id|>"),
        TEXT("<|start_header_id|>user<|end_header_id|>"),
        TEXT("<|start_header_id|>system<|end_header_id|>"),
        TEXT("<|start_header_id|>"),
        TEXT("<|end_header_id|>"),
        TEXT("</s>"),
        TEXT("[INST]"),
        TEXT("[/INST]")
    };

    for (const TCHAR* T : Tokens)
    {
        S.ReplaceInline(T, TEXT(""));
    }
}

static void LocalTalkerStripBracketTags(FString& S)
{
    if (S.IsEmpty()) return;

    FString Out;
    Out.Reserve(S.Len());

    for (int32 i = 0; i < S.Len();)
    {
        if (S[i] == TEXT('['))
        {
            const int32 CloseIdx = S.Find(TEXT("]"), ESearchCase::IgnoreCase, ESearchDir::FromStart, i + 1);
            if (CloseIdx != INDEX_NONE && (CloseIdx - i) <= 32)
            {
                FString Tag = S.Mid(i + 1, CloseIdx - i - 1);
                Tag.TrimStartAndEndInline();

                if (!Tag.IsEmpty())
                {
                    bool bTagOk = true;
                    for (int32 j = 0; j < Tag.Len(); j++)
                    {
                        const TCHAR C = Tag[j];
                        if (!(FChar::IsAlnum(C) || C == TEXT('_') || (j == 0 && C == TEXT('/'))))
                        {
                            bTagOk = false;
                            break;
                        }
                    }
                    if (bTagOk)
                    {
                        i = CloseIdx + 1;
                        continue;
                    }
                }
            }
        }

        Out.AppendChar(S[i]);
        i++;
    }

    S = Out;
}

static bool LocalTalkerTryExtractJsonStringField(const FString& In, const FString& Key, FString& OutValue)
{
    // Best-effort: extract `"Key":"..."`
    const FString Needle = FString::Printf(TEXT("\"%s\""), *Key);
    int32 KeyPos = In.Find(Needle, ESearchCase::IgnoreCase, ESearchDir::FromStart);
    if (KeyPos == INDEX_NONE) return false;

    int32 ColonPos = In.Find(TEXT(":"), ESearchCase::IgnoreCase, ESearchDir::FromStart, KeyPos + Needle.Len());
    if (ColonPos == INDEX_NONE) return false;

    int32 QuotePos = In.Find(TEXT("\""), ESearchCase::IgnoreCase, ESearchDir::FromStart, ColonPos + 1);
    if (QuotePos == INDEX_NONE) return false;

    FString Raw;
    Raw.Reserve(In.Len() - QuotePos);
    bool bEscape = false;
    for (int32 i = QuotePos + 1; i < In.Len(); i++)
    {
        const TCHAR C = In[i];
        if (bEscape)
        {
            Raw.AppendChar(TEXT('\\'));
            Raw.AppendChar(C);
            bEscape = false;
            continue;
        }
        if (C == TEXT('\\'))
        {
            bEscape = true;
            continue;
        }
        if (C == TEXT('"'))
        {
            break;
        }
        Raw.AppendChar(C);
    }

    // Unescape a minimal set
    FString S = Raw;
    S.ReplaceInline(TEXT("\\\\"), TEXT("\\"));
    S.ReplaceInline(TEXT("\\\""), TEXT("\""));
    S.ReplaceInline(TEXT("\\n"), TEXT("\n"));
    S.ReplaceInline(TEXT("\\r"), TEXT("\r"));
    S.ReplaceInline(TEXT("\\t"), TEXT("\t"));
    OutValue = S;
    return true;
}

static bool LocalTalkerTryExtractKeyLine(const FString& In, const FString& Key, FString& OutValue)
{
    TArray<FString> Lines;
    In.ParseIntoArrayLines(Lines, true);

    for (const FString& RawLine : Lines)
    {
        FString Line = RawLine;
        Line.TrimStartAndEndInline();
        if (!Line.StartsWith(Key, ESearchCase::IgnoreCase))
        {
            continue;
        }

        int32 Sep = Line.Find(TEXT("="));
        if (Sep == INDEX_NONE)
        {
            Sep = Line.Find(TEXT(":"));
        }
        if (Sep == INDEX_NONE)
        {
            continue;
        }

        OutValue = Line.Mid(Sep + 1);
        OutValue.TrimStartAndEndInline();
        if (!OutValue.IsEmpty())
        {
            return true;
        }
    }

    return false;
}

static bool LocalTalkerIsMetaLine(const FString& In)
{
    FString T = In;
    T.ReplaceInline(TEXT("\r"), TEXT(" "));
    T.ReplaceInline(TEXT("\n"), TEXT(" "));
    T.ReplaceInline(TEXT("\t"), TEXT(" "));
    while (T.Contains(TEXT("  "))) T.ReplaceInline(TEXT("  "), TEXT(" "));
    T.TrimStartAndEndInline();

    if (T.IsEmpty()) return true;

    if (T.StartsWith(TEXT("Role:"), ESearchCase::IgnoreCase) ||
        T.StartsWith(TEXT("Role "), ESearchCase::IgnoreCase) ||
        T.StartsWith(TEXT("You are now speaking as"), ESearchCase::IgnoreCase) ||
        T.StartsWith(TEXT("You are speaking now as"), ESearchCase::IgnoreCase) ||
        T.StartsWith(TEXT("Only output"), ESearchCase::IgnoreCase) ||
        T.StartsWith(TEXT("Write only"), ESearchCase::IgnoreCase) ||
        T.StartsWith(TEXT("Your turn"), ESearchCase::IgnoreCase) ||
        T.StartsWith(TEXT("Speak in-character"), ESearchCase::IgnoreCase) ||
        T.StartsWith(TEXT("Speak as"), ESearchCase::IgnoreCase) ||
        T.StartsWith(TEXT("The player has spoken"), ESearchCase::IgnoreCase) ||
        T.Contains(TEXT(" is speaking now"), ESearchCase::IgnoreCase) ||
        T.StartsWith(TEXT("DIRECTOR INSTRUCTION"), ESearchCase::IgnoreCase) ||
        T.StartsWith(TEXT("Instruction (do not repeat)"), ESearchCase::IgnoreCase) ||
        T.StartsWith(TEXT("Director instruction:"), ESearchCase::IgnoreCase) ||
        T.StartsWith(TEXT("Continue the conversation"), ESearchCase::IgnoreCase))
    {
        return true;
    }

    if (T.Contains(TEXT("stay in character"), ESearchCase::IgnoreCase) ||
        T.Contains(TEXT("keep the conversation"), ESearchCase::IgnoreCase))
    {
        return true;
    }

    if (T.StartsWith(TEXT("You are "), ESearchCase::IgnoreCase))
    {
        FString Rest = T.Mid(8);
        Rest.TrimStartAndEndInline();
        int32 SpaceIdx = Rest.Find(TEXT(" "));
        const FString First = (SpaceIdx == INDEX_NONE) ? Rest : Rest.Left(SpaceIdx);
        if (!First.IsEmpty() && FChar::IsUpper(First[0]) && First.Len() <= 16)
        {
            return true;
        }
    }

    return false;
}

static FString LocalTalkerCleanSpokenText(const FString& In)
{
    FString S = TrimSentence(In);
    if (S.IsEmpty()) return S;

    LocalTalkerStripControlTokens(S);
    LocalTalkerStripBracketTags(S);

    // If the model echoed our structured metadata, try to extract the "text" field.
    if (S.StartsWith(TEXT("{")) || S.Contains(TEXT("\"text\"")))
    {
        FString Extracted;
        if (LocalTalkerTryExtractJsonStringField(S, TEXT("text"), Extracted))
        {
            S = Extracted;
        }
    }

    // Extract from key/value style metadata: TEXT=... or Text: ...
    {
        FString Extracted;
        if (LocalTalkerTryExtractKeyLine(S, TEXT("TEXT"), Extracted))
        {
            S = Extracted;
        }
    }

    // If the model echoed our *old* plain-structured format, extract the Text: line.
    // Example:
    // Speaker: Assistant
    // FromUser: false
    // Text: Hello there!
    {
        const int32 TextPos = S.Find(TEXT("Text:"), ESearchCase::IgnoreCase, ESearchDir::FromStart);
        if (TextPos != INDEX_NONE)
        {
            S = S.Mid(TextPos + 5);
        }
    }

    // Strip surrounding quotes/braces that often happen when outputs are fragmented.
    S.TrimStartAndEndInline();

    // Strip leading ellipsis fragments (avoids separate ".." or "..." sentences).
    {
        int32 DotIdx = 0;
        while (DotIdx < S.Len() && (S[DotIdx] == TEXT('.') || S[DotIdx] == TCHAR(0x2026)))
        {
            DotIdx++;
        }
        int32 SpaceIdx = DotIdx;
        while (SpaceIdx < S.Len() && FChar::IsWhitespace(S[SpaceIdx]))
        {
            SpaceIdx++;
        }
        if (DotIdx > 0)
        {
            if (SpaceIdx >= S.Len())
            {
                return FString();
            }
            S = S.Mid(SpaceIdx);
            S.TrimStartAndEndInline();
        }
    }

    // Strip common transcript / role prefixes repeatedly ("User:", "Assistant:", "Otis:", etc.)
    // This is critical because if any prefix leaks into the context, the Director will propagate it forever.
    auto StripPrefix = [&S](const TCHAR* Prefix) -> bool
    {
        const int32 PrefixLen = FCString::Strlen(Prefix);
        if (S.Len() >= PrefixLen && S.Left(PrefixLen).Equals(Prefix, ESearchCase::IgnoreCase))
        {
            S = S.Mid(PrefixLen);
            S.TrimStartAndEndInline();
            return true;
        }
        return false;
    };

    for (;;)
    {
        bool bStripped = false;
        bStripped |= StripPrefix(TEXT("Assistant:"));
        bStripped |= StripPrefix(TEXT("User:"));
        bStripped |= StripPrefix(TEXT("System:"));
        bStripped |= StripPrefix(TEXT("Director:"));
        bStripped |= StripPrefix(TEXT("NPC:"));
        bStripped |= StripPrefix(TEXT("Speaker:"));
        bStripped |= StripPrefix(TEXT("FromUser:"));
        bStripped |= StripPrefix(TEXT("SPEAKER="));
        bStripped |= StripPrefix(TEXT("FROM_USER="));
        bStripped |= StripPrefix(TEXT("TEXT="));

        // Generic "Name:" prefix (single token up to 24 chars, no spaces) — catches "Milo:" / "Otis:" etc.
        int32 ColonIdx = S.Find(TEXT(":"), ESearchCase::IgnoreCase, ESearchDir::FromStart);
        if (ColonIdx > 0 && ColonIdx <= 24)
        {
            const FString Left = S.Left(ColonIdx);
            if (!Left.Contains(TEXT(" ")) && !Left.Contains(TEXT("\t")))
            {
                S = S.Mid(ColonIdx + 1);
                S.TrimStartAndEndInline();
                bStripped = true;
            }
        }

        auto StripBracketPrefix = [&S](TCHAR Open, TCHAR Close) -> bool
        {
            if (S.Len() < 3) return false;
            if (S[0] != Open) return false;
            const int32 CloseIdx = S.Find(FString::Chr(Close), ESearchCase::IgnoreCase, ESearchDir::FromStart, 1);
            if (CloseIdx <= 0 || CloseIdx > 32) return false;

            const FString Label = S.Mid(1, CloseIdx - 1);
            if (Label.Contains(TEXT(" ")) || Label.Contains(TEXT("\t")))
            {
                return false;
            }

            S = S.Mid(CloseIdx + 1);
            S.TrimStartAndEndInline();
            return true;
        };

        bStripped |= StripBracketPrefix(TEXT('['), TEXT(']'));
        bStripped |= StripBracketPrefix(TEXT('('), TEXT(')'));

        if (!bStripped) break;
    }

    // If the model emitted a trailing closing tag, remove it.
    {
        FString Trimmed = S;
        Trimmed.TrimStartAndEndInline();
        const int32 CloseIdx = Trimmed.Find(TEXT("[/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
        if (CloseIdx != INDEX_NONE)
        {
            const int32 EndIdx = Trimmed.Find(TEXT("]"), ESearchCase::IgnoreCase, ESearchDir::FromStart, CloseIdx + 2);
            if (EndIdx == Trimmed.Len() - 1)
            {
                Trimmed = Trimmed.Left(CloseIdx);
                Trimmed.TrimStartAndEndInline();
                S = Trimmed;
            }
        }
    }

    if (LocalTalkerIsMetaLine(S))
    {
        return FString();
    }

    // Drop common "metadata-only" junk.
    if (S.Equals(TEXT("Speaker:"), ESearchCase::IgnoreCase) ||
        S.StartsWith(TEXT("Speaker:"), ESearchCase::IgnoreCase) ||
        S.StartsWith(TEXT("FromUser:"), ESearchCase::IgnoreCase) ||
        S.StartsWith(TEXT("SPEAKER="), ESearchCase::IgnoreCase) ||
        S.StartsWith(TEXT("FROM_USER="), ESearchCase::IgnoreCase))
    {
        // If it's just those headers with no actual dialogue, ignore it completely.
        const FString One = LocalTalkerOneLine(S);
        if (One.Equals(TEXT("Speaker: Assistant"), ESearchCase::IgnoreCase) ||
            One.Equals(TEXT("Speaker: User"), ESearchCase::IgnoreCase) ||
            One.Equals(TEXT("Speaker: Director"), ESearchCase::IgnoreCase) ||
            One.StartsWith(TEXT("Speaker:"), ESearchCase::IgnoreCase) ||
            One.StartsWith(TEXT("FromUser:"), ESearchCase::IgnoreCase) ||
            One.StartsWith(TEXT("SPEAKER="), ESearchCase::IgnoreCase) ||
            One.StartsWith(TEXT("FROM_USER="), ESearchCase::IgnoreCase))
        {
            return FString();
        }
    }
    while (S.StartsWith(TEXT("\"")) && S.EndsWith(TEXT("\"")) && S.Len() >= 2)
    {
        S = S.Mid(1, S.Len() - 2);
        S.TrimStartAndEndInline();
    }
    while ((S == TEXT("\"}") || S == TEXT("}") || S == TEXT("\"") || S == TEXT("\"}"))) // common junk fragments
    {
        S.Reset();
        break;
    }

    // Collapse newlines/tabs -> spaces
    S.ReplaceInline(TEXT("\r"), TEXT(" "));
    S.ReplaceInline(TEXT("\n"), TEXT(" "));
    S.ReplaceInline(TEXT("\t"), TEXT(" "));
    while (S.Contains(TEXT("  "))) S.ReplaceInline(TEXT("  "), TEXT(" "));
    S.ReplaceInline(TEXT(" ."), TEXT("."));
    S.ReplaceInline(TEXT(" ,"), TEXT(","));
    S.ReplaceInline(TEXT(" !"), TEXT("!"));
    S.ReplaceInline(TEXT(" ?"), TEXT("?"));
    S.ReplaceInline(TEXT(" ;"), TEXT(";"));
    S.ReplaceInline(TEXT(" :"), TEXT(":"));
    S.TrimStartAndEndInline();

    if (!LocalTalkerHasAlphaNum(S))
    {
        return FString();
    }
    return S;
}

static FString LocalTalkerBriefFor(const ULocalCharacterComponent* C)
{
    if (!C) return FString();

    FString Brief = !C->Desc.IsEmpty() ? C->Desc : C->Directions;
    Brief = LocalTalkerOneLine(Brief);

    // Keep it short to avoid swamping the system prompt.
    const int32 MaxChars = 180;
    if (Brief.Len() > MaxChars)
    {
        Brief = Brief.Left(MaxChars) + TEXT("...");
    }
    return Brief;
}

static FString LocalTalkerBuildLlama3PromptFromContext(
    const FString& SelfSpeakerName,
    const FLocalTalkerCharacterConfig& C,
    const TArray<FLocalTalkMessage>& ContextHistory,
    const TArray<ULocalCharacterComponent*>& ContextParticipants,
    const FString& TurnPrompt // may be a Director instruction; user prompts are usually already in ContextHistory
)
{
    // Llama 3.x instruct template per llama.cpp chat templating docs:
    // https://github.com/ggml-org/llama.cpp/wiki/Templates-supported-by-llama_chat_apply_template
    auto AppendTurn = [](FString& P, const TCHAR* Role, const FString& Content)
    {
        P += TEXT("<|start_header_id|>");
        P += Role;
        P += TEXT("<|end_header_id|>\n\n");
        P += Content;
        P += TEXT("<|eot_id|>");
    };

    const FString TurnPromptClean = LocalTalkerOneLine(TurnPrompt);
    const bool bExplicitUserTurn =
        !TurnPromptClean.IsEmpty() &&
        !TurnPromptClean.StartsWith(TEXT("RAW:"), ESearchCase::IgnoreCase) &&
        !TurnPromptClean.StartsWith(TEXT("Respond to "), ESearchCase::IgnoreCase) &&
        !TurnPromptClean.StartsWith(TEXT("Director instruction:"), ESearchCase::IgnoreCase) &&
        !TurnPromptClean.StartsWith(TEXT("Instruction:"), ESearchCase::IgnoreCase);

    const FString SelfTag = LocalTalkerTagForName(SelfSpeakerName);

    // ---- System ----
    FString SystemBlock;
    const FString SelfName = SelfSpeakerName.IsEmpty() ? TEXT("the speaker") : SelfSpeakerName;
    SystemBlock += FString::Printf(TEXT("You are %s.\n\n"), *SelfName);
    SystemBlock += FString::Printf(TEXT("You must output EXACTLY ONE message from %s and nothing else.\n\n"), *SelfName);
    SystemBlock += TEXT("Output format (must match exactly):\n");
    SystemBlock += FString::Printf(TEXT("[%s] <dialogue> [/%s]\n\n"), *SelfTag, *SelfTag);
    SystemBlock += TEXT("Rules:\n");
    SystemBlock += FString::Printf(TEXT("- Your reply MUST begin with [%s] and end with [/%s].\n"), *SelfTag, *SelfTag);
    SystemBlock += TEXT("- Output only that single tagged block. No extra text before or after.\n");
    SystemBlock += FString::Printf(TEXT("- Do not output any other tags besides [%s] ... [/%s].\n"), *SelfTag, *SelfTag);
    SystemBlock += TEXT("- No narration, no actions, no stage directions.\n");
    SystemBlock += TEXT("- 1-3 sentences, natural and specific.\n");
    SystemBlock += TEXT("- Include exactly ONE concrete detail from the scene or context.\n");
    SystemBlock += FString::Printf(TEXT("- Do not repeat or paraphrase %s's last line.\n"), *SelfName);
    SystemBlock += TEXT("- Do not reuse any full sentence from the transcript.\n");
    SystemBlock += TEXT("- Do not reuse any 5+ word sequence from the transcript.\n");
    SystemBlock += TEXT("- If your draft matches any earlier line, discard it and write a different reply.\n");
    SystemBlock += TEXT("- Do not ask the same question twice; ask a new question with new wording.\n");

    // Keep system strictly for role + output rules. Character details go in the user block.

    // ---- History ----
    const int32 MaxMsgs = (C.MaxHistoryMessages > 0) ? C.MaxHistoryMessages : ContextHistory.Num();
    int32 StartIdx = 0;
    if (ContextHistory.Num() > MaxMsgs)
    {
        StartIdx = ContextHistory.Num() - MaxMsgs;
    }
    const int32 MaxChars = (C.MaxContextChars > 0) ? C.MaxContextChars : 1600;
    if (MaxChars > 0)
    {
        int64 BudgetUsed = 0;
        for (int32 i = StartIdx; i < ContextHistory.Num(); i++)
        {
            BudgetUsed += (int64)ContextHistory[i].SpeakerName.Len();
            BudgetUsed += (int64)ContextHistory[i].Content.Len();
            BudgetUsed += 8;
        }
        while (StartIdx < ContextHistory.Num() && BudgetUsed > (int64)MaxChars)
        {
            BudgetUsed -= (int64)ContextHistory[StartIdx].SpeakerName.Len();
            BudgetUsed -= (int64)ContextHistory[StartIdx].Content.Len();
            BudgetUsed -= 8;
            StartIdx++;
        }
    }

    // Keep track of the latest user utterance so player intent is always visible to the model.
    int32 LatestUserIdx = INDEX_NONE;
    FString LatestUserClean;
    for (int32 i = ContextHistory.Num() - 1; i >= 0; --i)
    {
        const FLocalTalkMessage& M = ContextHistory[i];
        if (!M.bFromUser)
        {
            continue;
        }
        const FString Clean = LocalTalkerOneLine(M.Content);
        if (Clean.IsEmpty() || LocalTalkerIsMetaLine(Clean))
        {
            continue;
        }
        LatestUserIdx = i;
        LatestUserClean = Clean;
        break;
    }

    TArray<FString> Others;
    for (ULocalCharacterComponent* P : ContextParticipants)
    {
        if (!P) continue;
        const FString Name = P->GetSpeakerNameResolved();
        if (Name.IsEmpty()) continue;
        if (Name.Equals(SelfSpeakerName, ESearchCase::IgnoreCase)) continue;
        if (!Others.Contains(Name))
        {
            Others.Add(Name);
        }
    }

    auto JoinNames = [](const TArray<FString>& Names) -> FString
    {
        if (Names.Num() == 0) return TEXT("someone nearby");
        if (Names.Num() == 1) return Names[0];
        if (Names.Num() == 2) return Names[0] + TEXT(" and ") + Names[1];
        FString Out;
        for (int32 i = 0; i < Names.Num(); i++)
        {
            if (i > 0)
            {
                Out += (i == Names.Num() - 1) ? TEXT(", and ") : TEXT(", ");
            }
            Out += Names[i];
        }
        return Out;
    };

    FString UserBlock;

    // Character descriptions block (user-side, per request).
    {
        TSet<FString> Seen;
        TArray<FString> Names;
        if (!SelfSpeakerName.IsEmpty())
        {
            Seen.Add(SelfSpeakerName);
            Names.Add(SelfSpeakerName);
        }
        for (ULocalCharacterComponent* P : ContextParticipants)
        {
            if (!P) continue;
            const FString Name = P->GetSpeakerNameResolved();
            if (Name.IsEmpty()) continue;
            if (!Seen.Contains(Name))
            {
                Seen.Add(Name);
                Names.Add(Name);
            }
        }
        for (const FLocalTalkMessage& M : ContextHistory)
        {
            if (M.SpeakerName.IsEmpty()) continue;
            if (M.SpeakerName.Equals(TEXT("User"), ESearchCase::IgnoreCase) ||
                M.SpeakerName.Equals(TEXT("Player"), ESearchCase::IgnoreCase))
            {
                continue;
            }
            if (!Seen.Contains(M.SpeakerName))
            {
                Seen.Add(M.SpeakerName);
                Names.Add(M.SpeakerName);
            }
        }

        if (Names.Num() > 0)
        {
            UserBlock += TEXT("Character Descriptions:\n");
            for (const FString& Name : Names)
            {
                const ULocalCharacterComponent* Match = nullptr;
                for (ULocalCharacterComponent* P : ContextParticipants)
                {
                    if (P && P->GetSpeakerNameResolved().Equals(Name, ESearchCase::IgnoreCase))
                    {
                        Match = P;
                        break;
                    }
                }

                FString Brief = LocalTalkerBriefFor(Match);
                if (Brief.IsEmpty())
                {
                    if (Name.Equals(SelfSpeakerName, ESearchCase::IgnoreCase))
                    {
                        const FString Desc = !C.CharacterDescription.IsEmpty() ? C.CharacterDescription : C.Persona;
                        Brief = Desc;
                    }
                }
                if (Brief.IsEmpty()) Brief = TEXT("(no description provided)");
                UserBlock += FString::Printf(TEXT("- %s: %s\n"), *Name, *Brief);
            }
            UserBlock += TEXT("\n");
        }
    }

    FString Transcript;
    Transcript += TEXT("[TRANSCRIPT]\n");
    bool bHasHistory = false;
    bool bIncludedLatestUser = false;
    FString LastLine;
    for (int32 i = StartIdx; i < ContextHistory.Num(); i++)
    {
        const FLocalTalkMessage& M = ContextHistory[i];
        const FString Clean = LocalTalkerOneLine(M.Content);
        if (Clean.IsEmpty()) continue;
        if (LocalTalkerIsMetaLine(Clean)) continue;

        const FString Tag = LocalTalkerTagForSpeakerName(M.SpeakerName, M.bFromUser);
        const FString Line = FString::Printf(TEXT("[%s] %s [/%s]"), *Tag, *Clean, *Tag);
        if (Line.Equals(LastLine, ESearchCase::IgnoreCase))
        {
            continue;
        }
        Transcript += Line + TEXT("\n");
        LastLine = Line;
        bHasHistory = true;
        if (M.bFromUser && i == LatestUserIdx)
        {
            bIncludedLatestUser = true;
        }
    }

    if (LatestUserIdx != INDEX_NONE && !bIncludedLatestUser && !LatestUserClean.IsEmpty())
    {
        const FString PlayerLine = FString::Printf(TEXT("[PLAYER] %s [/PLAYER]"), *LatestUserClean);
        if (!PlayerLine.Equals(LastLine, ESearchCase::IgnoreCase))
        {
            Transcript += PlayerLine + TEXT("\n");
            bHasHistory = true;
        }
    }
    Transcript += TEXT("[/TRANSCRIPT]\n\n");

    if (bHasHistory)
    {
        UserBlock += Transcript;
    }
    else
    {
        const FString Encounter = JoinNames(Others);
        UserBlock += TEXT("[TRANSCRIPT]\n");
        UserBlock += TEXT("[/TRANSCRIPT]\n\n");
        UserBlock += FString::Printf(TEXT("No transcript yet. You just ran into %s nearby.\n"), *Encounter);
        UserBlock += TEXT("Say a brief greeting to start the conversation.\n\n");
    }

    if (bExplicitUserTurn)
    {
        UserBlock += TEXT("Current player message to answer now:\n");
        UserBlock += FString::Printf(TEXT("[PLAYER] %s [/PLAYER]\n"), *TurnPromptClean);
        UserBlock += TEXT("You MUST directly address this message in your first sentence.\n\n");
    }
    else if (!LatestUserClean.IsEmpty())
    {
        UserBlock += TEXT("Latest player message:\n");
        UserBlock += FString::Printf(TEXT("[PLAYER] %s [/PLAYER]\n"), *LatestUserClean);
        UserBlock += TEXT("If relevant, naturally acknowledge this in your next reply.\n\n");
    }

    UserBlock += TEXT("Next speaker must be [");
    UserBlock += SelfTag;
    UserBlock += TEXT("].\n");
    UserBlock += TEXT("What would ");
    UserBlock += SelfSpeakerName.IsEmpty() ? TEXT("the speaker") : SelfSpeakerName;
    UserBlock += TEXT(" say next? Output only in the required ");
    UserBlock += FString::Printf(TEXT("[%s] ... [/%s]"), *SelfTag, *SelfTag);
    UserBlock += TEXT(" format.\n");

    FString Prompt;
    Prompt.Reserve(SystemBlock.Len() + 2048);
    Prompt += TEXT("<|begin_of_text|>");
    AppendTurn(Prompt, TEXT("system"), SystemBlock);
    AppendTurn(Prompt, TEXT("user"), UserBlock);

    // Generation prompt
    Prompt += TEXT("<|start_header_id|>assistant<|end_header_id|>\n\n");
    return Prompt;
}

static void LocalTalkerBuildStopSequencesForTags(
    const FString& SelfTag,
    const TArray<FLocalTalkMessage>& ContextHistory,
    const TArray<ULocalCharacterComponent*>& ContextParticipants,
    TArray<FString>& OutStops
)
{
    TSet<FString> Tags;
    Tags.Add(TEXT("PLAYER"));
    const FString SelfTagNorm = SelfTag.IsEmpty() ? TEXT("SPEAKER") : SelfTag;
    Tags.Add(SelfTagNorm);

    for (ULocalCharacterComponent* P : ContextParticipants)
    {
        if (!P) continue;
        Tags.Add(LocalTalkerTagForSpeakerName(P->GetSpeakerNameResolved(), false));
    }
    for (const FLocalTalkMessage& M : ContextHistory)
    {
        Tags.Add(LocalTalkerTagForSpeakerName(M.SpeakerName, M.bFromUser));
    }

    OutStops.Reset();
    OutStops.Add(TEXT("<|eot_id|>"));
    OutStops.Add(FString::Printf(TEXT("[/%s]"), *SelfTagNorm));
    OutStops.Add(TEXT("\n[PLAYER]"));
    OutStops.Add(TEXT("\nNext speaker must be"));
    OutStops.Add(TEXT("\n[TRANSCRIPT]"));
    OutStops.Add(TEXT("\n[/TRANSCRIPT]"));
    OutStops.Add(TEXT("\nWhat would "));
    OutStops.Add(TEXT("\n<|start_header_id|>"));
    OutStops.Add(TEXT("\n<|begin_of_text|>"));

    for (const FString& Tag : Tags)
    {
        OutStops.Add(FString::Printf(TEXT("\n[%s]"), *Tag));
    }
}

FString ULocalCharacterComponent::BuildPromptWithHistory(const FLocalTalkerCharacterConfig& Config, const FString& UserText) const
{
    // This helper remains for legacy/direct usage. For Director-managed multi-character prompting,
    // see InternalGrantTurn() which uses the Conversation Subsystem history + cast briefs.
    const FString ModelPath = ResolvePaths().LlamaModelPath;
    if (LocalTalkerModelLooksLikeLlama3(ModelPath))
    {
        TArray<FLocalTalkMessage> Context;
        Context.Add({ TEXT("User"), UserText, true });
        return LocalTalkerBuildLlama3PromptFromContext(GetSpeakerNameResolved(), Config, Context, /*Participants*/ {}, /*TurnPrompt*/ FString());
    }

    // Minimal fallback for non-llama3 models.
    FString P;
    P += TEXT("User: ") + UserText + TEXT("\nAssistant:");
    return P;
}

FLocalTalkerRuntimePaths ULocalCharacterComponent::ResolvePaths() const
{
    if (!bUseProjectSettingsPaths) return PathsOverride;

    const ULocalTalkerSettings* S = GetDefault<ULocalTalkerSettings>();
    FLocalTalkerRuntimePaths Out = S ? S->DefaultPaths : FLocalTalkerRuntimePaths();

    if (!PathsOverride.LlamaModelPath.IsEmpty()) Out.LlamaModelPath = PathsOverride.LlamaModelPath;
    if (!PathsOverride.LlamaLibPath.IsEmpty()) Out.LlamaLibPath = PathsOverride.LlamaLibPath;
    if (!PathsOverride.KokoroPythonExePath.IsEmpty()) Out.KokoroPythonExePath = PathsOverride.KokoroPythonExePath;
    if (!PathsOverride.KokoroWorkerScriptPath.IsEmpty()) Out.KokoroWorkerScriptPath = PathsOverride.KokoroWorkerScriptPath;
    if (!PathsOverride.KokoroCacheDir.IsEmpty()) Out.KokoroCacheDir = PathsOverride.KokoroCacheDir;
    if (!PathsOverride.WhisperPythonExePath.IsEmpty()) Out.WhisperPythonExePath = PathsOverride.WhisperPythonExePath;
    if (!PathsOverride.WhisperWorkerScriptPath.IsEmpty()) Out.WhisperWorkerScriptPath = PathsOverride.WhisperWorkerScriptPath;
    if (!PathsOverride.WhisperCacheDir.IsEmpty()) Out.WhisperCacheDir = PathsOverride.WhisperCacheDir;
    if (!PathsOverride.PiperExePath.IsEmpty()) Out.PiperExePath = PathsOverride.PiperExePath;
    if (!PathsOverride.PiperVoiceModelPath.IsEmpty()) Out.PiperVoiceModelPath = PathsOverride.PiperVoiceModelPath;
    if (!PathsOverride.WorkingDir.IsEmpty()) Out.WorkingDir = PathsOverride.WorkingDir;

    // Out-of-the-box defaults when shipping bundled artifacts with the plugin.
    // These are only used if the user hasn't set explicit paths in Project Settings/Overrides.
    if (Out.WorkingDir.IsEmpty())
    {
        if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LocalTalker")))
        {
            Out.WorkingDir = Plugin->GetBaseDir();
        }

    }

    if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LocalTalker")))
    {
        const FString Base = Plugin->GetBaseDir();

        if (Out.LlamaLibPath.IsEmpty())
        {
            Out.LlamaLibPath = FPaths::Combine(Base, TEXT("ThirdParty/llama/Win64/Release/libllama.dll"));
        }

        if (S && !S->BundledModelFile.IsEmpty())
        {
            FString Candidate = S->BundledModelFile;
            if (FPaths::IsRelative(Candidate))
            {
                Candidate = FPaths::Combine(Base, TEXT("Resources/Models"), Candidate);
            }
            if (FPaths::FileExists(Candidate))
            {
                Out.LlamaModelPath = Candidate;
            }
        }

        if (Out.LlamaModelPath.IsEmpty())
        {
            // Default shipped model path (we'll bundle at least one small, permissive GGUF)
            Out.LlamaModelPath = FPaths::Combine(Base, TEXT("Resources/Models/Llama-3.2-3B-Instruct-Q6_K_L.gguf"));
        }

        if (Out.KokoroWorkerScriptPath.IsEmpty())
        {
            Out.KokoroWorkerScriptPath = FPaths::Combine(Base, TEXT("Resources/Kokoro/kokoro_tts_worker.py"));
        }

        if (Out.KokoroPythonExePath.IsEmpty())
        {
            Out.KokoroPythonExePath = TEXT("python");
        }

        if (Out.WhisperWorkerScriptPath.IsEmpty())
        {
            Out.WhisperWorkerScriptPath = FPaths::Combine(Base, TEXT("Resources/Whisper/whisper_stt_worker.py"));
        }

        if (Out.WhisperPythonExePath.IsEmpty())
        {
            Out.WhisperPythonExePath = TEXT("python");
        }
    }

    return Out;
}

FLocalTalkerCharacterConfig ULocalCharacterComponent::ResolveConfig() const
{
    if (!bUseProjectSettingsConfig) return CharacterConfigOverride;

    const ULocalTalkerSettings* S = GetDefault<ULocalTalkerSettings>();
    FLocalTalkerCharacterConfig Out = S ? S->DefaultCharacterConfig : FLocalTalkerCharacterConfig();

    if (!CharacterConfigOverride.Directions.IsEmpty()) Out.Directions = CharacterConfigOverride.Directions;
    if (!CharacterConfigOverride.CharacterDescription.IsEmpty()) Out.CharacterDescription = CharacterConfigOverride.CharacterDescription;

    if (!CharacterConfigOverride.SystemPrompt.IsEmpty()) Out.SystemPrompt = CharacterConfigOverride.SystemPrompt;
    if (!CharacterConfigOverride.Persona.IsEmpty()) Out.Persona = CharacterConfigOverride.Persona;

    if (!Desc.IsEmpty() && Out.Persona.IsEmpty() && Out.CharacterDescription.IsEmpty())
    {
        Out.Persona = Desc;
    }

    Out.MaxContextChars = CharacterConfigOverride.MaxContextChars != 1600 ? CharacterConfigOverride.MaxContextChars : Out.MaxContextChars;
    Out.MaxHistoryMessages = CharacterConfigOverride.MaxHistoryMessages != 16 ? CharacterConfigOverride.MaxHistoryMessages : Out.MaxHistoryMessages;

    Out.MaxTokens = CharacterConfigOverride.MaxTokens != 64 ? CharacterConfigOverride.MaxTokens : Out.MaxTokens;
    Out.Temperature = CharacterConfigOverride.Temperature != 0.4f ? CharacterConfigOverride.Temperature : Out.Temperature;
    Out.Seed = CharacterConfigOverride.Seed != 0 ? CharacterConfigOverride.Seed : Out.Seed;

    if (!CharacterConfigOverride.Stop.IsEmpty()) Out.Stop = CharacterConfigOverride.Stop;
    if (CharacterConfigOverride.StopSequences.Num() > 0) Out.StopSequences = CharacterConfigOverride.StopSequences;

    Out.bSpeak = CharacterConfigOverride.bSpeak;
    Out.bStreamTokens = CharacterConfigOverride.bStreamTokens;

    return Out;
}

void ULocalCharacterComponent::Interrupt()
{
    bInterrupted = true;
    bLLMFinished = true;
    bAudioPlaybackComplete = true;
    bSpokeThisTurn = false;

    if (ActiveLLM)
    {
        ActiveLLM->Cancel();
    }

    LLMTextBuffer.Reset();
    LLMFullText.Reset();

    FString Tmp;
    while (SentenceQueue.Dequeue(Tmp)) {}

    PendingSentenceCount.Reset();
    bAudioPlaybackComplete = true;
    PendingAudioWave = nullptr;
    PendingSubtitleText.Reset();
    PendingSubtitleDurationSeconds = 0.0f;

    if (AudioComp) AudioComp->Stop();

    ActiveLLM = nullptr;

    // Best-effort clear: QueueSubtitles is exported; KillSubtitles is not.
    if (bUseUESubtitles)
    {
        const PTRINT SubtitleId = (PTRINT)this;
        TWeakObjectPtr<UWorld> WorldPtr = GetWorld();
        AsyncTask(ENamedThreads::GameThread, [SubtitleId, WorldPtr]()
        {
            UWorld* World = WorldPtr.Get();
            if (!World) return;

            TArray<FSubtitleCue> Cues;
            FSubtitleCue Cue;
            Cue.Text = FText::GetEmpty();
            Cue.Time = 0.0f;
            Cues.Add(Cue);

            FSubtitleManager::GetSubtitleManager()->QueueSubtitles(
                SubtitleId,
                /*Priority*/ 1000.0f,
                /*bManualWordWrap*/ false,
                /*bSingleLine*/ true,
                /*SoundDuration*/ 0.05f,
                Cues,
                /*InStartTime*/ 0.0f,
                World->GetAudioTimeSeconds()
            );
        });
    }
}

void ULocalCharacterComponent::ClearConversation()
{
    if (UWorld* W = GetWorld())
    {
        if (auto* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            Sub->ClearContextHistory(this);
        }
    }
}

void ULocalCharacterComponent::SpeakTextLocal(const FString& Text)
{
    if (!ShouldAllowTalk())
    {
        UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Skipping speech (no player listener in range)."),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved());
        return;
    }

    bInterrupted = false;
    bLLMFinished = true;
    bSpokeThisTurn = false;
    bNotifiedSubsystemFinished = false;
    bAudioPlaybackComplete = true;
    PendingAudioWave = nullptr;
    PendingSubtitleText.Reset();
    PendingSubtitleDurationSeconds = 0.0f;
    ActiveAudioStartWorldSeconds = 0.0;
    ActiveAudioDurationSeconds = 0.0f;

    EnsureAudio();
    EnqueueSentence(Text);

    const FLocalTalkerRuntimePaths Paths = ResolvePaths();
    StartTTSWorker(Paths);
}

void ULocalCharacterComponent::SendPromptAndSpeakStreamingInProc(const FString& Prompt)
{
    if (UWorld* W = GetWorld())
    {
        if (auto* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            Sub->RequestTurn(this, Prompt);
            return;
        }
    }

    // Fallback: if Director subsystem isn't present, run directly.
    InternalGrantTurn(Prompt);
}

void ULocalCharacterComponent::InternalGrantTurn(const FString& PromptOrText)
{
    if (!ShouldAllowTalk())
    {
        UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Skipping turn (no player listener in range)."),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved());
        return;
    }

    bInterrupted = false;
    bLLMFinished = false;
    bSpokeThisTurn = false;
    bNotifiedSubsystemFinished = false;
    bAudioPlaybackComplete = true;
    PendingAudioWave = nullptr;
    PendingSubtitleText.Reset();
    PendingSubtitleDurationSeconds = 0.0f;
    ActiveAudioStartWorldSeconds = 0.0;
    ActiveAudioDurationSeconds = 0.0f;
    PendingSentenceCount.Reset();

    LLMTextBuffer.Reset();
    LLMFullText.Reset();
    LastTextAppendSeconds = FPlatformTime::Seconds();

    EnsureAudio();

    const FLocalTalkerRuntimePaths Paths = ResolvePaths();
    FLocalTalkerCharacterConfig Config = ResolveConfig();
    StartTTSWorker(Paths);

    // RAW: bypass LLM and speak directly.
    if (PromptOrText.StartsWith(TEXT("RAW:"), ESearchCase::IgnoreCase))
    {
        const FString Raw = PromptOrText.Mid(4);
        SpeakTextLocal(Raw);
        bLLMFinished = true;
        return;
    }

    // Build a full prompt using the active context (multi-character) when available.
    FString PromptText;
    TArray<FLocalTalkMessage> ContextHistory;
    TArray<ULocalCharacterComponent*> Participants;
    if (UWorld* W = GetWorld())
    {
        if (auto* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            ContextHistory = Sub->GetContextHistory(this);
            Participants = Sub->GetContextParticipants(this);
            int32 UserMsgs = 0;
            FString LastUserPreview;
            for (int32 i = ContextHistory.Num() - 1; i >= 0; --i)
            {
                if (!ContextHistory[i].bFromUser)
                {
                    continue;
                }
                UserMsgs++;
                if (LastUserPreview.IsEmpty())
                {
                    LastUserPreview = LocalTalkerPreview(LocalTalkerOneLine(ContextHistory[i].Content));
                }
            }
            UE_LOG(LogLocalTalker, Log,
                TEXT("%s[%s] Building LLM prompt with history=%d participants=%d userMsgs=%d lastUser=\"%s\""),
                *LocalTalkerTimePrefix(this),
                *GetSpeakerNameResolved(),
                ContextHistory.Num(),
                Participants.Num(),
                UserMsgs,
                LastUserPreview.IsEmpty() ? TEXT("<none>") : *LastUserPreview);
            if (LocalTalkerModelLooksLikeLlama3(Paths.LlamaModelPath))
            {
                PromptText = LocalTalkerBuildLlama3PromptFromContext(
                    GetSpeakerNameResolved(),
                    Config,
                    ContextHistory,
                    Participants,
                    PromptOrText
                );
            }
        }
    }

    // If we couldn't build a context-aware prompt, fall back to legacy prompt builder.
    if (PromptText.IsEmpty())
    {
        PromptText = BuildPromptWithHistory(Config, PromptOrText);
    }

    if (LocalTalkerModelLooksLikeLlama3(Paths.LlamaModelPath) &&
        Config.StopSequences.Num() == 0 &&
        Config.Stop.IsEmpty())
    {
        const FString SelfTag = LocalTalkerTagForName(GetSpeakerNameResolved());
        LocalTalkerBuildStopSequencesForTags(SelfTag, ContextHistory, Participants, Config.StopSequences);
    }

    LocalTalkerDumpPromptToFile(GetSpeakerNameResolved(), PromptText);

    ActiveLLM = ULocalTalkerInProcGenerateAsync::GenerateStreamingInProcWithPromptText(this, Paths, Config, PromptText);
    ActiveLLM->OnToken.AddDynamic(this, &ULocalCharacterComponent::HandleLLMToken);
    ActiveLLM->OnDelta.AddDynamic(this, &ULocalCharacterComponent::HandleLLMDelta);
    ActiveLLM->OnCompleted.AddDynamic(this, &ULocalCharacterComponent::HandleLLMCompleted);
    ActiveLLM->OnError.AddDynamic(this, &ULocalCharacterComponent::HandleLLMError);
    ActiveLLM->Activate();
}

bool ULocalCharacterComponent::ShouldAllowTalk() const
{
    const ULocalTalkerSettings* S = GetDefault<ULocalTalkerSettings>();
    if (!S || !S->bRequirePlayerListenerForAllTalk)
    {
        return true;
    }

    if (UWorld* W = GetWorld())
    {
        if (auto* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            return Sub->HasPlayerListenerInRange(this);
        }
    }

    return true;
}

void ULocalCharacterComponent::OnHeardSpeech(const FString& InSpeakerName, const FString& Text, bool bFromUser)
{
    // The Director owns the authoritative context history. This callback is for local reactions / logging.
    FString OneLine = LocalTalkerOneLine(Text);
    const int32 MaxChars = 120;
    if (OneLine.Len() > MaxChars) OneLine = OneLine.Left(MaxChars) + TEXT("...");
    UE_LOG(LogLocalTalker, Verbose, TEXT("%s[%s] Heard (%s) from %s: %s"),
        *LocalTalkerTimePrefix(this),
        *GetSpeakerNameResolved(),
        bFromUser ? TEXT("User") : TEXT("NPC"),
        *InSpeakerName,
        *OneLine
    );
}

void ULocalCharacterComponent::HandleLLMError(const FString& Error)
{
    if (bInterrupted)
    {
        UE_LOG(LogLocalTalker, Verbose, TEXT("%s[%s] LLM canceled after interrupt: %s"), *LocalTalkerTimePrefix(this), *GetSpeakerNameResolved(), *Error);
        bLLMFinished = true;
        return;
    }

    UE_LOG(LogLocalTalker, Error, TEXT("%s[%s] LLM error: %s"), *LocalTalkerTimePrefix(this), *GetSpeakerNameResolved(), *Error);
    if (bDebugPrintGeneratedText)
    {
        DebugPrintLine(FString::Printf(TEXT("[%s] LLM ERROR: %s"), *GetSpeakerNameResolved(), *Error), 6.0f, /*bNewLine*/ false);
    }
    OnError.Broadcast(Error);
    bLLMFinished = true;
}

void ULocalCharacterComponent::HandleLLMToken(const FString& Token)
{
    if (bDebugLogTokens)
    {
        UE_LOG(LogLocalTalker, Verbose, TEXT("%s[%s] token: %s"), *LocalTalkerTimePrefix(this), *GetSpeakerNameResolved(), *Token);
    }
    OnToken.Broadcast(Token);
}

void ULocalCharacterComponent::HandleLLMDelta(const FString& Text)
{
    if (bInterrupted) return;
    if (Text.IsEmpty()) return;

    LLMTextBuffer += Text;
    LLMFullText += Text;
    LastTextAppendSeconds = FPlatformTime::Seconds();

    if (bDebugPrintGeneratedText)
    {
        // Show a short rolling window so it stays readable.
        const int32 MaxChars = 140;
        const FString Tail = (LLMFullText.Len() > MaxChars) ? LLMFullText.Right(MaxChars) : LLMFullText;
        DebugPrintLine(FString::Printf(TEXT("[%s] %s"), *GetSpeakerNameResolved(), *Tail), 1.0f, /*bNewLine*/ false);
    }
}

void ULocalCharacterComponent::HandleLLMCompleted(const FString& Text)
{
    // Text is the full completion (per async node contract).
    bLLMFinished = true;
    OnSpokenText.Broadcast(Text);
    // Flush any trailing text so it gets spoken/broadcast even if it didn't hit a terminator.
    if (!Text.IsEmpty())
    {
        if (bSpeakStreaming)
        {
            ExtractAndEnqueueSentences(/*bForceFlush*/ true);
        }
        else
        {
            EnqueueSentence(Text);
            LLMTextBuffer.Reset();
        }
    }
}

void ULocalCharacterComponent::EnqueueSentenceInternal(const FString& Sentence, bool bBroadcast)
{
    const FString S = LocalTalkerCleanSpokenText(Sentence);
    if (S.Len() <= 0) return;

    // PendingSentenceCount represents sentences that still need TTS completion (including in-flight worker synthesis).
    PendingSentenceCount.Increment();

    SentenceQueue.Enqueue(S);
    if (bBroadcast)
    {
        bSpokeThisTurn = true;
        if (UWorld* W = GetWorld())
        {
            if (auto* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
            {
                Sub->BroadcastSentence(this, S, /*bFromUser*/ false);
            }
        }
    }
    UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Enqueued sentence (%d chars, pending=%d): %s"),
        *LocalTalkerTimePrefix(this),
        *GetSpeakerNameResolved(),
        S.Len(),
        PendingSentenceCount.GetValue(),
        *LocalTalkerPreview(S));
}

void ULocalCharacterComponent::EnqueueSentence(const FString& Sentence)
{
    EnqueueSentenceInternal(Sentence, /*bBroadcast*/ true);
}

void ULocalCharacterComponent::ExtractAndEnqueueSentences(bool bForceFlush)
{
    if (!bSpeakStreaming) return;
    if (LLMTextBuffer.Len() < MinCharsBeforeSpeak && !bForceFlush) return;

    FString Work = LLMTextBuffer;
    Work.ReplaceInline(TEXT("\r"), TEXT(""));

    int32 CutIdx = INDEX_NONE;
    int32 LastSoftIdx = INDEX_NONE;
    int32 LastWordBoundaryIdx = INDEX_NONE;
    int32 WordCount = 0;
    bool bInWord = false;
    for (int32 i = 0; i < Work.Len(); i++)
    {
        const TCHAR C = Work[i];
        const bool bSpace = FChar::IsWhitespace(C);
        if (!bSpace)
        {
            if (!bInWord)
            {
                WordCount++;
                bInWord = true;
            }
        }
        else
        {
            bInWord = false;
        }

        if (bSpace || C == TEXT(',') || C == TEXT(';') || C == TEXT(':') || C == TEXT('.') || C == TEXT('!') || C == TEXT('?'))
        {
            LastWordBoundaryIdx = i;
        }

        if (IsSentenceTerminator(Work[i]))
        {
            if (Work[i] == TEXT('.') && IsEllipsisAt(Work, i))
            {
                continue;
            }
            CutIdx = i;
            if (i + 1 >= MinCharsBeforeSpeak) break;
        }

        if (bAllowPhraseChunks && i + 1 >= MinCharsBeforeSpeak)
        {
            const bool bHasMinWords = (MinWordsBeforeSpeak > 0) ? (WordCount >= MinWordsBeforeSpeak) : true;
            if (bHasMinWords && (C == TEXT(',') || C == TEXT(';') || C == TEXT(':')))
            {
                LastSoftIdx = i;
            }
            if (MaxWordsBeforeSpeak > 0 && WordCount >= MaxWordsBeforeSpeak)
            {
                CutIdx = (LastSoftIdx != INDEX_NONE) ? LastSoftIdx : i;
                break;
            }
        }

        if (i >= MaxSentenceChars)
        {
            CutIdx = (LastSoftIdx != INDEX_NONE && LastSoftIdx >= MinCharsBeforeSpeak) ? LastSoftIdx : i;
            break;
        }

    }

    if (CutIdx == INDEX_NONE)
    {
        if (bForceFlush && Work.Len() > 0)
        {
            const bool bAllowShortFlush = bLLMFinished;
            const bool bLongEnough = Work.Len() >= MinCharsBeforeSpeak;
            if (bAllowShortFlush || bLongEnough)
            {
                EnqueueSentence(Work);
                LLMTextBuffer.Reset();
            }
        }
        return;
    }

    if (CutIdx != INDEX_NONE && LastWordBoundaryIdx != INDEX_NONE && CutIdx > LastWordBoundaryIdx)
    {
        CutIdx = LastWordBoundaryIdx;
    }

    const int32 TakeLen = CutIdx + 1;
    const FString Sentence = Work.Left(TakeLen);
    const FString Remainder = Work.Mid(TakeLen);

    EnqueueSentence(Sentence);
    LLMTextBuffer = Remainder;
}

class FLocalTalkerTTSWorker : public FRunnable
{
public:
    FLocalTalkerTTSWorker(
        TQueue<FString, EQueueMode::Mpsc>& InSentenceQueue,
        FThreadSafeBool& InStop,
        ULocalCharacterComponent* InOwner,
        const FLocalTalkerRuntimePaths& InPaths
    )
        : SentenceQueue(InSentenceQueue)
        , bStop(InStop)
        , Owner(InOwner)
        , Paths(InPaths)
    {}

    virtual uint32 Run() override
    {
        if (Owner && LocalTalkerShouldLogAudioTrace())
        {
            UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] TTS worker thread started."),
                *LocalTalkerTimePrefix(Owner),
                *Owner->GetSpeakerNameResolved());
        }

        while (!bStop)
        {
            FString Sentence;
            if (!SentenceQueue.Dequeue(Sentence))
            {
                FPlatformProcess::Sleep(0.01f);
                continue;
            }

            if (bStop) break;
            if (!Owner) break;

            if (LocalTalkerShouldLogAudioTrace())
            {
                UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] TTS worker dequeued sentence (%d chars, pending=%d): %s"),
                    *LocalTalkerTimePrefix(Owner),
                    *Owner->GetSpeakerNameResolved(),
                    Sentence.Len(),
                    Owner->PendingSentenceCount.GetValue(),
                    *LocalTalkerPreview(Sentence));
            }

            FString Err;
            {
                Owner->RunKokoroSentenceToAudio(Sentence, Paths, Err);
            }
            if (!Err.IsEmpty())
            {
                UE_LOG(LogLocalTalker, Error, TEXT("%s[%s] TTS worker sentence failed: %s"),
                    *LocalTalkerTimePrefix(Owner),
                    *Owner->GetSpeakerNameResolved(),
                    *Err);
                AsyncTask(ENamedThreads::GameThread, [Owner = Owner, Err]()
                {
                    if (Owner) Owner->OnError.Broadcast(Err);
                });
            }

            // Mark this sentence as fully processed (either produced audio or errored).
            Owner->PendingSentenceCount.Decrement();
            const int32 PendingNow = Owner->PendingSentenceCount.GetValue();
            if (PendingNow < 0)
            {
                // Can happen if an interrupt/reset cleared pending count while a worker task was in flight.
                Owner->PendingSentenceCount.Reset();
                UE_LOG(LogLocalTalker, Warning, TEXT("%s[%s] Pending sentence counter underflow corrected (%d -> 0)."),
                    *LocalTalkerTimePrefix(Owner),
                    *Owner->GetSpeakerNameResolved(),
                    PendingNow);
            }
            if (LocalTalkerShouldLogAudioTrace())
            {
                UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] TTS worker completed sentence (pending now=%d)."),
                    *LocalTalkerTimePrefix(Owner),
                    *Owner->GetSpeakerNameResolved(),
                    Owner->PendingSentenceCount.GetValue());
            }
        }
        if (Owner && LocalTalkerShouldLogAudioTrace())
        {
            UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] TTS worker thread stopped."),
                *LocalTalkerTimePrefix(Owner),
                *Owner->GetSpeakerNameResolved());
        }
        return 0;
    }

private:
    TQueue<FString, EQueueMode::Mpsc>& SentenceQueue;
    FThreadSafeBool& bStop;
    ULocalCharacterComponent* Owner = nullptr;
    FLocalTalkerRuntimePaths Paths;
};

void ULocalCharacterComponent::StartTTSWorker(const FLocalTalkerRuntimePaths& Paths)
{
    if (bTTSWorkerRunning)
    {
        if (LocalTalkerShouldLogAudioTrace())
        {
            UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] StartTTSWorker skipped: worker already running."),
                *LocalTalkerTimePrefix(this),
                *GetSpeakerNameResolved());
        }
        return;
    }

    bTTSWorkerRunning = true;
    bTTSStop = false;

    if (LocalTalkerShouldLogAudioTrace())
    {
        UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Starting TTS worker (backend=Kokoro, py='%s', script='%s')."),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved(),
            *Paths.KokoroPythonExePath,
            *Paths.KokoroWorkerScriptPath);
    }

    TTSRunnable = new FLocalTalkerTTSWorker(SentenceQueue, bTTSStop, this, Paths);
    TTSThread = FRunnableThread::Create(TTSRunnable, TEXT("LocalTalkerTTSWorker"), 0, TPri_BelowNormal);
    if (!TTSThread)
    {
        UE_LOG(LogLocalTalker, Error, TEXT("%s[%s] Failed to create TTS worker thread."),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved());
        bTTSWorkerRunning = false;
        bTTSStop = true;
        if (TTSRunnable)
        {
            delete TTSRunnable;
            TTSRunnable = nullptr;
        }
    }
}

void ULocalCharacterComponent::StopTTSWorker()
{
    if (!bTTSWorkerRunning) return;

    if (LocalTalkerShouldLogAudioTrace())
    {
        UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Stopping TTS worker."),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved());
    }

    bTTSWorkerRunning = false;
    bTTSStop = true;

    if (TTSThread)
    {
        TTSThread->WaitForCompletion();
        delete TTSThread;
        TTSThread = nullptr;
    }

    if (TTSRunnable)
    {
        delete TTSRunnable;
        TTSRunnable = nullptr;
    }

    ShutdownKokoroWorker();
}

void ULocalCharacterComponent::TryStartPendingAudio()
{
    const bool bTraceAudio = LocalTalkerShouldLogAudioTrace();

    if (bInterrupted) return;
    if (!PendingAudioWave) return;

    EnsureAudio();
    if (!AudioComp)
    {
        UE_LOG(LogLocalTalker, Error, TEXT("%s[%s] TryStartPendingAudio aborted: no AudioComp."),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved());
        return;
    }
    if (AudioComp->IsPlaying()) return;
    if (IsAudioBlockedByOtherSpeaker())
    {
        if (!bWaitingForOtherSpeaker)
        {
            BlockedSinceSeconds = FPlatformTime::Seconds();
            if (bTraceAudio)
            {
                UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] TryStartPendingAudio queued: waiting for other speaker to finish."),
                    *LocalTalkerTimePrefix(this),
                    *GetSpeakerNameResolved());
            }
        }
        bWaitingForOtherSpeaker = true;
        return;
    }
    if (bWaitingForOtherSpeaker)
    {
        if (bTraceAudio)
        {
            UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] TryStartPendingAudio unblocked after %.1fs."),
                *LocalTalkerTimePrefix(this),
                *GetSpeakerNameResolved(),
                FPlatformTime::Seconds() - BlockedSinceSeconds);
        }
        bWaitingForOtherSpeaker = false;
    }

    AudioComp->SetSound(PendingAudioWave);
    if (bTraceAudio)
    {
        UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] TryStartPendingAudio play request: wave=%p dur=%.2fs subtitleChars=%d"),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved(),
            PendingAudioWave,
            PendingSubtitleDurationSeconds,
            PendingSubtitleText.Len());
    }
    AudioComp->Play();
    bAudioPlaybackComplete = false;
    ActiveAudioStartWorldSeconds = 0.0;
    ActiveAudioDurationSeconds = PendingSubtitleDurationSeconds;
    if (UWorld* W = GetWorld())
    {
        ActiveAudioStartWorldSeconds = W->GetTimeSeconds();
    }

    if (!PendingSubtitleText.IsEmpty())
    {
        const float DurationSec = PendingSubtitleDurationSeconds;
        const float SubtitleDuration = (DurationSec > 0.0f) ? (DurationSec + FMath::Max(0.0f, AudioCompletionGraceSeconds)) : DurationSec;
        if (bUseUESubtitles && SubtitleDuration > 0.0f)
        {
            const PTRINT SubtitleId = (PTRINT)this;
            const float Priority = UESubtitlePriority;
            const FString Line = FString::Printf(TEXT("%s: %s"), *GetSpeakerNameResolved(), *PendingSubtitleText);
            if (UWorld* World = GetWorld())
            {
                TArray<FSubtitleCue> Cues;
                FSubtitleCue Cue;
                Cue.Text = FText::FromString(Line);
                Cue.Time = 0.0f;
                Cues.Add(Cue);

                FSubtitleManager::GetSubtitleManager()->QueueSubtitles(
                    SubtitleId,
                    Priority,
                    /*bManualWordWrap*/ false,
                    /*bSingleLine*/ true,
                    SubtitleDuration,
                    Cues,
                    /*InStartTime*/ 0.0f,
                    World->GetAudioTimeSeconds()
                );
            }
        }

        EmitSubtitle(PendingSubtitleText, SubtitleDuration);
    }

    UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Audio started (full)."), *LocalTalkerTimePrefix(this), *GetSpeakerNameResolved());
    if (bTraceAudio && !AudioComp->IsPlaying())
    {
        UE_LOG(LogLocalTalker, Warning, TEXT("%s[%s] AudioComp->Play called, but IsPlaying is false immediately after start."),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved());
    }

    PendingAudioWave = nullptr;
    PendingSubtitleText.Reset();
    PendingSubtitleDurationSeconds = 0.0f;
}

void ULocalCharacterComponent::HandleAudioFinished()
{
    if (bAudioPlaybackComplete) return;
    bAudioPlaybackComplete = true;
    ActiveAudioStartWorldSeconds = 0.0;
    ActiveAudioDurationSeconds = 0.0f;
    UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Audio stopped (completed). pendingWave=%p"),
        *LocalTalkerTimePrefix(this),
        *GetSpeakerNameResolved(),
        PendingAudioWave);
    TryStartPendingAudio();
    if (UWorld* W = GetWorld())
    {
        if (auto* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            Sub->NotifyAudioFinished(this);
        }
    }
}

bool ULocalCharacterComponent::IsAudioBlockedByOtherSpeaker() const
{
    UWorld* W = GetWorld();
    if (!W) return false;

    ULocalTalkConversationSubsystem* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>();
    if (!Sub) return false;

    const TArray<ULocalCharacterComponent*> Participants = Sub->GetContextParticipants(this);
    for (ULocalCharacterComponent* P : Participants)
    {
        if (P && P != this && P->IsAudioPlaying())
        {
            return true;
        }
    }
    return false;
}

// =============================================================================
// Kokoro ONNX TTS backend
// =============================================================================

bool ULocalCharacterComponent::ResolveKokoroVoiceSelection(FString& OutVoice) const
{
    const ULocalTalkerSettings* Settings = GetDefault<ULocalTalkerSettings>();
    if (!Settings) { OutVoice = TEXT("af_bella"); return true; }

    if (!VoiceId.IsNone())
    {
        for (const FLocalTalkVoiceOption& Opt : Settings->Voices)
        {
            if (Opt.Id == VoiceId && !Opt.KokoroVoice.IsEmpty())
            {
                OutVoice = Opt.KokoroVoice;
                return true;
            }
        }
    }

    OutVoice = Settings->KokoroDefaultVoice.IsEmpty() ? TEXT("af_bella") : Settings->KokoroDefaultVoice;
    return true;
}

void ULocalCharacterComponent::ShutdownKokoroWorker()
{
    // No per-component state; Kokoro uses the global shared pool.
}

void ULocalCharacterComponent::ShutdownSharedTtsWorkerGlobal()
{
    FScopeLock SharedLock(&GLocalTalkerSharedKokoroWorkerMutex);
    LocalTalkerShutdownSharedTtsWorkerNoLock(GLocalTalkerSharedKokoroWorker);
}

bool ULocalCharacterComponent::EnsureKokoroWorker(const FLocalTalkerRuntimePaths& Paths, FString& OutErr)
{
    OutErr.Reset();

    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LocalTalker"));
    const FString PluginBase = Plugin.IsValid() ? Plugin->GetBaseDir() : FString();

    // Resolve worker script path
    FString ScriptPath = Paths.KokoroWorkerScriptPath;
    if (ScriptPath.IsEmpty())
    {
        ScriptPath = PluginBase.IsEmpty()
            ? TEXT("Resources/Kokoro/kokoro_tts_worker.py")
            : FPaths::Combine(PluginBase, TEXT("Resources/Kokoro/kokoro_tts_worker.py"));
    }
    else if (FPaths::IsRelative(ScriptPath))
    {
        // Convert to absolute first (handles paths already relative to CWD, e.g. set by ResolvePaths).
        const FString Full = FPaths::ConvertRelativePathToFull(ScriptPath);
        if (FPaths::FileExists(Full))
        {
            ScriptPath = Full;
        }
        else
        {
            const FString Candidate = PluginBase.IsEmpty() ? FString() : FPaths::Combine(PluginBase, ScriptPath);
            ScriptPath = (!Candidate.IsEmpty() && FPaths::FileExists(Candidate))
                ? FPaths::ConvertRelativePathToFull(Candidate)
                : Full;
        }
    }

    if (!FPaths::FileExists(ScriptPath))
    {
        OutErr = FString::Printf(TEXT("Kokoro worker script not found: %s"), *ScriptPath);
        return false;
    }

    const FString PythonExe = Paths.KokoroPythonExePath.IsEmpty() ? TEXT("python") : Paths.KokoroPythonExePath;
    if (PythonExe.IsEmpty())
    {
        OutErr = TEXT("KokoroPythonExePath is empty. Set it in Project Settings -> LocalTalker -> DefaultPaths.");
        return false;
    }

    const ULocalTalkerSettings* Settings = GetDefault<ULocalTalkerSettings>();
    const double Timeout    = Settings ? FMath::Max(5.0, (double)Settings->KokoroRequestTimeoutSeconds) : 60.0;
    const float  Speed      = Settings ? FMath::Clamp(Settings->KokoroSpeed, 0.5f, 2.0f) : 1.0f;
    const int32  PoolSize   = Settings ? FMath::Clamp(Settings->KokoroWorkerPoolSize, 1, 4) : 1;
    const FString CacheDir  = Paths.KokoroCacheDir;

    FScopeLock SharedLock(&GLocalTalkerSharedKokoroWorkerMutex);

    const FString WorkerKey = LocalTalkerBuildKokoroWorkerKey(PythonExe, ScriptPath, CacheDir, Speed, PoolSize);

    // Check if existing pool is still healthy
    if (GLocalTalkerSharedKokoroWorker.IsValid() &&
        GLocalTalkerSharedKokoroWorker->WorkerKey == WorkerKey &&
        GLocalTalkerSharedKokoroWorker->Workers.Num() == PoolSize)
    {
        bool bAllRunning = true;
        for (const TUniquePtr<FLocalTtsWorkerPoolEntry>& Entry : GLocalTalkerSharedKokoroWorker->Workers)
        {
            if (!Entry.IsValid() || !Entry->Worker.Handle.IsValid() ||
                !FPlatformProcess::IsProcRunning(Entry->Worker.Handle))
            {
                bAllRunning = false;
                break;
            }
        }
        if (bAllRunning)
        {
            return true;
        }
    }

    // Shutdown stale pool
    if (GLocalTalkerSharedKokoroWorker.IsValid())
    {
        for (TUniquePtr<FLocalTtsWorkerPoolEntry>& Entry : GLocalTalkerSharedKokoroWorker->Workers)
        {
            if (Entry.IsValid()) LocalTalkerShutdownWorkerStateNoLock(Entry->Worker);
        }
        GLocalTalkerSharedKokoroWorker->Workers.Reset();
        GLocalTalkerSharedKokoroWorker.Reset();
    }

    // Spawn new pool
    GLocalTalkerSharedKokoroWorker = MakeUnique<FLocalTalkerSharedTtsWorkerState>();
    GLocalTalkerSharedKokoroWorker->WorkerKey = WorkerKey;

    for (int32 i = 0; i < PoolSize; ++i)
    {
        FString Args = FString::Printf(TEXT("\"%s\""), *ScriptPath);
        if (!CacheDir.IsEmpty()) Args += FString::Printf(TEXT(" --cache-dir \"%s\""), *CacheDir);
        Args += FString::Printf(TEXT(" --speed %.2f"), Speed);

        UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Kokoro worker spawn [%d/%d]: python='%s' script='%s'"),
            *LocalTalkerTimePrefix(this), *GetSpeakerNameResolved(), i + 1, PoolSize, *PythonExe, *ScriptPath);

        auto NewEntry = MakeUnique<FLocalTtsWorkerPoolEntry>();
        FString SpawnErr;
        if (!FLocalTalkerProcess::SpawnWithPipes(PythonExe, Args, Paths.WorkingDir,
            NewEntry->Worker.Handle, NewEntry->Worker.Pipes, SpawnErr))
        {
            OutErr = FString::Printf(TEXT("Failed to spawn Kokoro worker [%d]: %s"), i + 1, *SpawnErr);
            LocalTalkerShutdownWorkerStateNoLock(NewEntry->Worker);
            continue;
        }

        UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Kokoro worker spawned pid=%u"),
            *LocalTalkerTimePrefix(this), *GetSpeakerNameResolved(),
            FPlatformProcess::GetCurrentProcessId());

        // Send health check and wait for "ready"
        const FString HealthReq = TEXT("{\"cmd\":\"health\"}\n");
        if (!FLocalTalkerProcess::WriteStdin(NewEntry->Worker.Pipes, HealthReq))
        {
            OutErr = TEXT("Failed to write health request to Kokoro worker stdin.");
            LocalTalkerShutdownWorkerStateNoLock(NewEntry->Worker);
            continue;
        }

        const double HealthStart = FPlatformTime::Seconds();
        bool bReady = false;
        while ((FPlatformTime::Seconds() - HealthStart) < Timeout)
        {
            // Drain stderr (download progress etc.)
            const FString ErrOut = FLocalTalkerProcess::ReadAvailable(NewEntry->Worker.Pipes.ReadErrPipe);
            if (!ErrOut.IsEmpty())
            {
                TArray<FString> ErrLines;
                ErrOut.ParseIntoArrayLines(ErrLines, false);
                for (const FString& ELine : ErrLines)
                {
                    FString Trimmed = ELine;
                    Trimmed.TrimStartAndEndInline();
                    if (!Trimmed.IsEmpty())
                        UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Kokoro worker: %s"),
                            *LocalTalkerTimePrefix(this), *GetSpeakerNameResolved(), *Trimmed);
                }
            }

            const FString StdOut = FLocalTalkerProcess::ReadAvailable(NewEntry->Worker.Pipes.ReadPipe);
            if (!StdOut.IsEmpty())
            {
                NewEntry->Worker.StdoutBuffer += StdOut;
                FString Line;
                if (LocalTalkerTryPopLine(NewEntry->Worker.StdoutBuffer, Line))
                {
                    TSharedPtr<FJsonObject> Parsed;
                    if (LocalTalkerParseJsonLine(Line, Parsed) && Parsed.IsValid())
                    {
                        bool bOk = false, bReadyFlag = false;
                        Parsed->TryGetBoolField(TEXT("ok"), bOk);
                        Parsed->TryGetBoolField(TEXT("ready"), bReadyFlag);
                        if (bOk && bReadyFlag)
                        {
                            FString BackendStr;
                            Parsed->TryGetStringField(TEXT("backend"), BackendStr);
                            const TArray<TSharedPtr<FJsonValue>>* SpeakersArr = nullptr;
                            int32 NumVoices = 0;
                            if (Parsed->TryGetArrayField(TEXT("speakers"), SpeakersArr) && SpeakersArr)
                                NumVoices = SpeakersArr->Num();
                            UE_LOG(LogLocalTalker, Log,
                                TEXT("%s[%s] Kokoro worker ready. backend='%s' voices=%d"),
                                *LocalTalkerTimePrefix(this), *GetSpeakerNameResolved(),
                                *BackendStr, NumVoices);
                            bReady = true;
                        }
                        else
                        {
                            FString ErrMsg;
                            Parsed->TryGetStringField(TEXT("error"), ErrMsg);
                            OutErr = FString::Printf(TEXT("Kokoro health check failed: %s"), *ErrMsg);
                        }
                    }
                    break;
                }
            }

            if (!FPlatformProcess::IsProcRunning(NewEntry->Worker.Handle))
            {
                OutErr = TEXT("Kokoro worker process died during startup.");
                break;
            }
            FPlatformProcess::Sleep(0.05f);
        }

        if (!bReady)
        {
            if (OutErr.IsEmpty()) OutErr = TEXT("Kokoro worker health check timed out.");
            LocalTalkerShutdownWorkerStateNoLock(NewEntry->Worker);
            continue;
        }

        GLocalTalkerSharedKokoroWorker->Workers.Add(MoveTemp(NewEntry));
    }

    if (GLocalTalkerSharedKokoroWorker->Workers.IsEmpty())
    {
        GLocalTalkerSharedKokoroWorker.Reset();
        if (OutErr.IsEmpty()) OutErr = TEXT("Failed to start any Kokoro worker processes.");
        return false;
    }

    return true;
}

void ULocalCharacterComponent::RunKokoroSentenceToAudio(
    const FString& Sentence,
    const FLocalTalkerRuntimePaths& Paths,
    FString& OutErr)
{
    const ULocalTalkerSettings* Settings = GetDefault<ULocalTalkerSettings>();

    if (!EnsureKokoroWorker(Paths, OutErr))
    {
        UE_LOG(LogLocalTalker, Error, TEXT("%s[%s] EnsureKokoroWorker failed: %s"),
            *LocalTalkerTimePrefix(this), *GetSpeakerNameResolved(), *OutErr);
        return;
    }

    UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Kokoro TTS start: %s"),
        *LocalTalkerTimePrefix(this), *GetSpeakerNameResolved(), *Sentence);

    FString Voice;
    ResolveKokoroVoiceSelection(Voice);

    const FString ReqId = FString::Printf(TEXT("%s-%llu"), *GetSpeakerNameResolved(), (uint64)FPlatformTime::Cycles64());

    TSharedRef<FJsonObject> Req = MakeShared<FJsonObject>();
    Req->SetStringField(TEXT("cmd"),        TEXT("synthesize"));
    Req->SetStringField(TEXT("text"),       Sentence);
    Req->SetStringField(TEXT("speaker"),    Voice);
    Req->SetStringField(TEXT("request_id"), ReqId);
    Req->SetBoolField  (TEXT("streaming"),  true);

    FString ReqLine;
    if (!LocalTalkerSerializeJsonLine(Req, ReqLine))
    {
        OutErr = TEXT("Failed to serialize Kokoro request.");
        return;
    }

    UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Kokoro synth opts: voice='%s'"),
        *LocalTalkerTimePrefix(this), *GetSpeakerNameResolved(), *Voice);

    // --- Select least-busy worker entry ---
    FLocalTtsWorkerPoolEntry* SelectedEntry = nullptr;
    {
        FScopeLock SharedLock(&GLocalTalkerSharedKokoroWorkerMutex);
        if (GLocalTalkerSharedKokoroWorker.IsValid())
        {
            int32 BestInFlight = MAX_int32;
            for (const TUniquePtr<FLocalTtsWorkerPoolEntry>& Entry : GLocalTalkerSharedKokoroWorker->Workers)
            {
                if (!Entry.IsValid() || !Entry->Worker.Handle.IsValid() ||
                    !FPlatformProcess::IsProcRunning(Entry->Worker.Handle))
                    continue;
                const int32 InFlight = Entry->InFlightRequests.GetValue();
                if (!SelectedEntry || InFlight < BestInFlight) { SelectedEntry = Entry.Get(); BestInFlight = InFlight; }
            }
        }
    }
    if (!SelectedEntry)
    {
        OutErr = TEXT("Kokoro worker is not running.");
        return;
    }
    SelectedEntry->InFlightRequests.Increment();

    // Shared flag: while > 0 the underflow callback fills silence to keep wave alive between chunks.
    TSharedPtr<FThreadSafeCounter> StreamingAlive = MakeShared<FThreadSafeCounter>();
    StreamingAlive->Set(1);

    {
        FScopeLock RequestLock(&SelectedEntry->RequestMutex);
        FLocalTtsWorkerState& WS = SelectedEntry->Worker;

        if (!FLocalTalkerProcess::WriteStdin(WS.Pipes, ReqLine))
        {
            OutErr = TEXT("Failed to write request to Kokoro worker stdin.");
            SelectedEntry->InFlightRequests.Decrement();
            return;
        }

        const FString TimePrefix  = LocalTalkerTimePrefix(this);
        const FString SpeakerLabel = GetSpeakerNameResolved();
        const double Timeout      = Settings ? FMath::Max(5.0, (double)Settings->KokoroRequestTimeoutSeconds) : 60.0;
        const double Start        = FPlatformTime::Seconds();
        const bool bTraceAudio    = LocalTalkerShouldLogAudioTrace();

        USoundWaveProcedural* StreamWave   = nullptr;
        int32 StreamSR        = 24000;
        int32 StreamCh        = 1;
        int32 TotalStreamedBytes = 0;
        int32 ChunksReceived  = 0;
        bool  bDone           = false;

        while (!bDone)
        {
            if (bTTSStop || bInterrupted)
            {
                OutErr = TEXT("Kokoro synthesis canceled.");
                break;
            }

            // Read stdout
            const FString StdoutChunk = FLocalTalkerProcess::ReadAvailable(WS.Pipes.ReadPipe);
            if (!StdoutChunk.IsEmpty())
            {
                WS.StdoutBuffer += StdoutChunk;
                FString Line;
                while (LocalTalkerTryPopLine(WS.StdoutBuffer, Line))
                {
                    if (Line.IsEmpty()) continue;
                    TSharedPtr<FJsonObject> Parsed;
                    if (!LocalTalkerParseJsonLine(Line, Parsed) || !Parsed.IsValid()) continue;

                    bool bIsChunk = false;
                    Parsed->TryGetBoolField(TEXT("streaming_chunk"), bIsChunk);

                    if (bIsChunk)
                    {
                        FString PcmB64;
                        Parsed->TryGetStringField(TEXT("pcm_base64"), PcmB64);
                        TArray<uint8> ChunkBytes;
                        if (!FBase64::Decode(PcmB64, ChunkBytes) || ChunkBytes.Num() < 2) continue;

                        double RespSR = 0, RespCh = 0;
                        Parsed->TryGetNumberField(TEXT("sample_rate"), RespSR);
                        Parsed->TryGetNumberField(TEXT("num_channels"), RespCh);
                        StreamSR = FMath::Max(1, (int32)RespSR);
                        StreamCh = FMath::Max(1, (int32)RespCh);
                        const int32 ChunkSize = ChunkBytes.Num();

                        if (!StreamWave)
                        {
                            // First chunk: create SoundWaveProcedural on game thread and start playback
                            const FString SubtitleText = Sentence;
                            const float EstDuration = FMath::Max(2.0f, (float)Sentence.Len() * 0.08f);
                            const int32 CapturedSR = StreamSR;
                            const int32 CapturedCh = StreamCh;

                            FEvent* WaveReady = FPlatformProcess::GetSynchEventFromPool(false);
                            AsyncTask(ENamedThreads::GameThread, [this, &StreamWave, WaveReady, ChunkData = MoveTemp(ChunkBytes), CapturedSR, CapturedCh, SubtitleText, EstDuration, StreamingAlive]() mutable
                            {
                                EnsureAudio();
                                if (!AudioComp) { WaveReady->Trigger(); return; }

                                USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(this, TEXT("LocalTalkerKokoroWave"));
                                Wave->bLooping = false;
                                Wave->Duration = INDEFINITELY_LOOPING_DURATION;
                                Wave->SampleByteSize = sizeof(int16);
                                Wave->NumChannels = CapturedCh;
                                Wave->SetSampleRate(CapturedSR);

                                // Inject silence on underflow to bridge inter-chunk gaps.
                                // When streaming is done (StreamingAlive==0), do nothing so the wave
                                // drains its buffered PCM. The timer-based fallback in TickComponent
                                // will call Stop() once the actual audio duration has elapsed.
                                Wave->OnSoundWaveProceduralUnderflow.BindLambda([StreamingAlive, CapturedCh](USoundWaveProcedural* InWave, int32 SamplesNeeded)
                                {
                                    if (StreamingAlive.IsValid() && StreamingAlive->GetValue() > 0)
                                    {
                                        const int32 BytesNeeded = SamplesNeeded * CapturedCh * (int32)sizeof(int16);
                                        TArray<uint8> Silence;
                                        Silence.SetNumZeroed(BytesNeeded);
                                        InWave->QueueAudio(Silence.GetData(), Silence.Num());
                                    }
                                });

                                Wave->QueueAudio(ChunkData.GetData(), ChunkData.Num());
                                StreamWave = Wave;
                                PendingAudioWave = Wave;
                                PendingSubtitleText = SubtitleText;
                                PendingSubtitleDurationSeconds = EstDuration;
                                bAudioPlaybackComplete = false;
                                TryStartPendingAudio();
                                WaveReady->Trigger();
                            });
                            WaveReady->Wait();
                            FPlatformProcess::ReturnSynchEventToPool(WaveReady);

                            UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Kokoro first chunk: %d bytes, %d Hz, playback started"),
                                *TimePrefix, *SpeakerLabel, ChunkSize, CapturedSR);
                        }
                        else
                        {
                            StreamWave->QueueAudio(ChunkBytes.GetData(), ChunkBytes.Num());
                            if (bTraceAudio)
                            {
                                UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Kokoro chunk %d queued: %d bytes"),
                                    *TimePrefix, *SpeakerLabel, ChunksReceived, ChunkSize);
                            }
                        }

                        TotalStreamedBytes += ChunkSize;
                        ChunksReceived++;
                        continue;
                    }

                    bool bStreamDone = false;
                    Parsed->TryGetBoolField(TEXT("streaming_done"), bStreamDone);
                    if (bStreamDone)
                    {
                        const TSharedPtr<FJsonObject>* MetricsObj = nullptr;
                        if (Parsed->TryGetObjectField(TEXT("metrics"), MetricsObj) && MetricsObj && MetricsObj->IsValid())
                        {
                            double ModelSeconds = 0, TotalSeconds = 0;
                            (*MetricsObj)->TryGetNumberField(TEXT("model_seconds"), ModelSeconds);
                            (*MetricsObj)->TryGetNumberField(TEXT("total_seconds"), TotalSeconds);
                            UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Kokoro metrics: req_id='%s' chunks=%d model=%.2fs total=%.2fs"),
                                *TimePrefix, *SpeakerLabel, *ReqId, ChunksReceived, ModelSeconds, TotalSeconds);
                        }
                        bDone = true;
                        break;
                    }

                    // Fallback: non-streaming response
                    bool bOk = false;
                    Parsed->TryGetBoolField(TEXT("ok"), bOk);
                    FString FallbackPcm;
                    Parsed->TryGetStringField(TEXT("pcm_base64"), FallbackPcm);
                    if (bOk && !FallbackPcm.IsEmpty())
                    {
                        TArray<uint8> FallbackBytes;
                        if (FBase64::Decode(FallbackPcm, FallbackBytes) && FallbackBytes.Num() >= 2)
                        {
                            double RespSR = 0, RespCh = 0;
                            Parsed->TryGetNumberField(TEXT("sample_rate"), RespSR);
                            Parsed->TryGetNumberField(TEXT("num_channels"), RespCh);
                            StreamSR = FMath::Max(1, (int32)RespSR);
                            StreamCh = FMath::Max(1, (int32)RespCh);
                            TotalStreamedBytes = FallbackBytes.Num();
                            const float DurationSec = (StreamSR > 0) ? ((float)FallbackBytes.Num() / (float)(2 * StreamCh * StreamSR)) : 0.0f;
                            const FString SubtitleText = Sentence;
                            AsyncTask(ENamedThreads::GameThread, [this, Bytes = MoveTemp(FallbackBytes), SR = StreamSR, Ch = StreamCh, DurationSec, SubtitleText]() mutable
                            {
                                EnsureAudio();
                                if (!AudioComp) return;
                                USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(this, TEXT("LocalTalkerKokoroProcWave"));
                                Wave->bLooping = false;
                                Wave->Duration = INDEFINITELY_LOOPING_DURATION;
                                Wave->SampleByteSize = sizeof(int16);
                                Wave->NumChannels = Ch;
                                Wave->SetSampleRate(SR);
                                Wave->QueueAudio(Bytes.GetData(), Bytes.Num());
                                PendingAudioWave = Wave;
                                PendingSubtitleText = SubtitleText;
                                PendingSubtitleDurationSeconds = DurationSec;
                                bAudioPlaybackComplete = false;
                                TryStartPendingAudio();
                            });
                        }
                        bDone = true;
                        break;
                    }

                    if (Parsed->HasField(TEXT("error")))
                    {
                        Parsed->TryGetStringField(TEXT("error"), OutErr);
                        bDone = true;
                        break;
                    }
                }
            }

            // Read stderr
            const FString ErrChunk = FLocalTalkerProcess::ReadAvailable(WS.Pipes.ReadErrPipe);
            if (!ErrChunk.IsEmpty())
            {
                TArray<FString> ErrLines;
                ErrChunk.ParseIntoArrayLines(ErrLines, false);
                for (const FString& RawLine : ErrLines)
                {
                    FString ELine = RawLine;
                    ELine.TrimStartAndEndInline();
                    if (ELine.IsEmpty()) continue;
                    UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Kokoro worker: %s"), *TimePrefix, *SpeakerLabel, *ELine);
                }
            }

            if (!FPlatformProcess::IsProcRunning(WS.Handle))
            {
                OutErr = TEXT("Kokoro worker died during synthesis.");
                break;
            }
            if ((FPlatformTime::Seconds() - Start) >= Timeout)
            {
                OutErr = FString::Printf(TEXT("Kokoro synthesis timed out (%.1fs)."), Timeout);
                break;
            }

            FPlatformProcess::Sleep(0.005f);
        }

        // All chunks done. Clear underflow flag so the wave drains its buffered PCM.
        StreamingAlive->Set(0);

        const float DurationSec = (StreamSR > 0 && StreamCh > 0)
            ? ((float)TotalStreamedBytes / (float)(2 * StreamCh * StreamSR)) : 0.0f;

        // Replace the rough char-count estimate with the real PCM duration so TickComponent's
        // timer fallback fires at the right time, even when audio started while blocked.
        if (DurationSec > 0.0f)
        {
            AsyncTask(ENamedThreads::GameThread, [WeakThis = TWeakObjectPtr<ULocalCharacterComponent>(this), DurationSec]()
            {
                ULocalCharacterComponent* Owner = WeakThis.Get();
                if (!Owner || Owner->bAudioPlaybackComplete) return;
                Owner->PendingSubtitleDurationSeconds = DurationSec;
                // If playback has already started, patch the live timer too.
                if (Owner->ActiveAudioStartWorldSeconds > 0.0 && Owner->ActiveAudioDurationSeconds > 0.0f)
                {
                    Owner->ActiveAudioDurationSeconds = DurationSec;
                }
            });
        }

        UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Kokoro TTS done: %d bytes, %d chunks, %.2fs audio, %d Hz"),
            *TimePrefix, *SpeakerLabel, TotalStreamedBytes, ChunksReceived, DurationSec, StreamSR);
    }

    SelectedEntry->InFlightRequests.Decrement();
}
