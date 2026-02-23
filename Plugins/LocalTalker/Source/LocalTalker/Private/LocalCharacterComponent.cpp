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

struct FLocalQwenWorkerState
{
    FProcHandle Handle;
    FLocalProcPipes Pipes;
    FString StdoutBuffer;
    FString StderrBuffer;
};

struct FLocalQwenWorkerPoolEntry
{
    FLocalQwenWorkerState Worker;
    FCriticalSection RequestMutex;
    FThreadSafeCounter InFlightRequests;
};

struct FLocalTalkerSharedQwenWorkerState
{
    FString WorkerKey;
    TArray<TUniquePtr<FLocalQwenWorkerPoolEntry>> Workers;
};

static bool LocalTalkerTryPopLine(FString& InOutBuffer, FString& OutLine);
static bool LocalTalkerParseJsonLine(const FString& Line, TSharedPtr<FJsonObject>& OutObj);

static TUniquePtr<FLocalTalkerSharedQwenWorkerState> GLocalTalkerSharedQwenWorker;
static FCriticalSection GLocalTalkerSharedQwenWorkerMutex;
static bool GLocalTalkerQwenPrewarmStarted = false;
static TSet<FString> GLocalTalkerSeenBenignQwenStderr;
static FCriticalSection GLocalTalkerBenignStderrMutex;

static void LocalTalkerShutdownWorkerStateNoLock(FLocalQwenWorkerState& Worker)
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

static void LocalTalkerShutdownSharedQwenWorkerNoLock()
{
    if (!GLocalTalkerSharedQwenWorker.IsValid())
    {
        return;
    }

    const double WaitStart = FPlatformTime::Seconds();
    while (true)
    {
        int32 TotalInFlight = 0;
        for (const TUniquePtr<FLocalQwenWorkerPoolEntry>& Entry : GLocalTalkerSharedQwenWorker->Workers)
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

    for (TUniquePtr<FLocalQwenWorkerPoolEntry>& Entry : GLocalTalkerSharedQwenWorker->Workers)
    {
        if (Entry.IsValid())
        {
            LocalTalkerShutdownWorkerStateNoLock(Entry->Worker);
        }
    }
    GLocalTalkerSharedQwenWorker->Workers.Reset();
    GLocalTalkerSharedQwenWorker.Reset();
    GLocalTalkerQwenPrewarmStarted = false;
}

static bool LocalTalkerIsBenignQwenStderrLine(const FString& InLine)
{
    FString Line = InLine;
    Line.TrimStartAndEndInline();
    if (Line.IsEmpty())
    {
        return true;
    }

    return
        Line.Contains(TEXT("'sox' is not recognized"), ESearchCase::IgnoreCase) ||
        Line.Contains(TEXT("operable program or batch file"), ESearchCase::IgnoreCase) ||
        Line.Contains(TEXT("SoX could not be found"), ESearchCase::IgnoreCase) ||
        Line.Contains(TEXT("sox.sourceforge.net"), ESearchCase::IgnoreCase) ||
        Line.Contains(TEXT("If you do not have SoX"), ESearchCase::IgnoreCase) ||
        Line.Contains(TEXT("If you do (or think that you should) have SoX"), ESearchCase::IgnoreCase) ||
        Line.Contains(TEXT("path variables"), ESearchCase::IgnoreCase) ||
        Line.Contains(TEXT("Loading Qwen3-TTS model"), ESearchCase::IgnoreCase) ||
        Line.Contains(TEXT("Tokenizer override is currently informational"), ESearchCase::IgnoreCase) ||
        Line.Contains(TEXT("Model loaded. kind="), ESearchCase::IgnoreCase) ||
        Line.Contains(TEXT("Setting `pad_token_id` to `eos_token_id`"), ESearchCase::IgnoreCase) ||
        Line.Contains(TEXT("generation flags are not valid"), ESearchCase::IgnoreCase);
}

static FString LocalTalkerBuildQwenWorkerKey(
    const FString& PythonExe,
    const FString& WorkerScriptPath,
    const FString& ModelPath,
    const FString& TokenizerPath,
    const FString& Device,
    const FString& DType,
    const FString& WorkingDir,
    bool bUseFlashAttn,
    int32 WorkerPoolSize)
{
    return FString::Printf(TEXT("%s|%s|%s|%s|%s|%s|%s|flash=%d|pool=%d"),
        *PythonExe,
        *WorkerScriptPath,
        *ModelPath,
        *TokenizerPath,
        *Device,
        *DType,
        *WorkingDir,
        bUseFlashAttn ? 1 : 0,
        WorkerPoolSize);
}

static bool LocalTalkerLooksLikeRepoId(const FString& InPath)
{
    if (InPath.IsEmpty())
    {
        return false;
    }
    if (InPath.Contains(TEXT("://")))
    {
        return false;
    }
    if (FPaths::FileExists(InPath) || FPaths::DirectoryExists(InPath))
    {
        return false;
    }

    FString Norm = InPath;
    Norm.ReplaceInline(TEXT("\\"), TEXT("/"));
    if (!Norm.Contains(TEXT("/")))
    {
        return false;
    }
    if (Norm.StartsWith(TEXT("./")) || Norm.StartsWith(TEXT("../")))
    {
        return false;
    }
    return true;
}

static void LocalTalkerAddUniqueNonEmpty(TArray<FString>& Paths, const FString& Path)
{
    if (!Path.IsEmpty())
    {
        Paths.AddUnique(Path);
    }
}

static bool LocalTalkerTryResolveHuggingFaceSnapshot(const FString& RepoId, FString& OutSnapshotDir)
{
    OutSnapshotDir.Reset();
    if (!LocalTalkerLooksLikeRepoId(RepoId))
    {
        return false;
    }

    FString RepoEscaped = RepoId;
    RepoEscaped.ReplaceInline(TEXT("\\"), TEXT("/"));
    RepoEscaped.ReplaceInline(TEXT("/"), TEXT("--"));

    TArray<FString> HubRoots;
    LocalTalkerAddUniqueNonEmpty(HubRoots, FPlatformMisc::GetEnvironmentVariable(TEXT("HUGGINGFACE_HUB_CACHE")));

    const FString HFHome = FPlatformMisc::GetEnvironmentVariable(TEXT("HF_HOME"));
    if (!HFHome.IsEmpty())
    {
        LocalTalkerAddUniqueNonEmpty(HubRoots, FPaths::Combine(HFHome, TEXT("hub")));
    }

    const FString UserProfile = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
    if (!UserProfile.IsEmpty())
    {
        LocalTalkerAddUniqueNonEmpty(HubRoots, FPaths::Combine(UserProfile, TEXT(".cache/huggingface/hub")));
    }

    const FString LocalAppData = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));
    if (!LocalAppData.IsEmpty())
    {
        LocalTalkerAddUniqueNonEmpty(HubRoots, FPaths::Combine(LocalAppData, TEXT("huggingface/hub")));
    }

    IFileManager& FileManager = IFileManager::Get();
    for (const FString& HubRoot : HubRoots)
    {
        const FString SnapshotsDir = FPaths::Combine(HubRoot, FString::Printf(TEXT("models--%s"), *RepoEscaped), TEXT("snapshots"));
        if (!FileManager.DirectoryExists(*SnapshotsDir))
        {
            continue;
        }

        TArray<FString> SnapshotFolders;
        FileManager.FindFiles(SnapshotFolders, *FPaths::Combine(SnapshotsDir, TEXT("*")), false, true);
        if (SnapshotFolders.Num() == 0)
        {
            continue;
        }

        FDateTime BestTimestamp = FDateTime::MinValue();
        FString BestPath;
        for (const FString& FolderName : SnapshotFolders)
        {
            const FString Candidate = FPaths::Combine(SnapshotsDir, FolderName);
            const FDateTime Timestamp = FileManager.GetTimeStamp(*Candidate);
            if (BestPath.IsEmpty() || Timestamp > BestTimestamp)
            {
                BestPath = Candidate;
                BestTimestamp = Timestamp;
            }
        }

        if (!BestPath.IsEmpty() && FileManager.DirectoryExists(*BestPath))
        {
            OutSnapshotDir = BestPath;
            return true;
        }
    }

    return false;
}

static FString LocalTalkerResolveQwenAssetPath(const FString& InPath, const FString& PluginBaseDir, bool& bOutResolvedViaSnapshot)
{
    bOutResolvedViaSnapshot = false;

    FString Path = InPath;
    if (Path.IsEmpty())
    {
        return Path;
    }

    if (FPaths::FileExists(Path) || FPaths::DirectoryExists(Path))
    {
        return Path;
    }

    if (FPaths::IsRelative(Path))
    {
        if (!PluginBaseDir.IsEmpty())
        {
            const FString PluginCandidate = FPaths::Combine(PluginBaseDir, Path);
            if (FPaths::FileExists(PluginCandidate) || FPaths::DirectoryExists(PluginCandidate))
            {
                return PluginCandidate;
            }
        }

        const FString FullCandidate = FPaths::ConvertRelativePathToFull(Path);
        if (FPaths::FileExists(FullCandidate) || FPaths::DirectoryExists(FullCandidate))
        {
            return FullCandidate;
        }
    }

    FString SnapshotPath;
    if (LocalTalkerTryResolveHuggingFaceSnapshot(Path, SnapshotPath))
    {
        bOutResolvedViaSnapshot = true;
        return SnapshotPath;
    }

    return Path;
}

static FString LocalTalkerGetDefaultQwenSpeaker()
{
    const ULocalTalkerSettings* S = GetDefault<ULocalTalkerSettings>();
    if (S)
    {
        for (const FLocalTalkVoiceOption& V : S->Voices)
        {
            if (!V.QwenSpeaker.IsEmpty())
            {
                return V.QwenSpeaker;
            }
        }
    }
    // Safe fallback for Qwen3 custom voice models.
    return TEXT("aiden");
}

static bool LocalTalkerSendQwenWorkerRequestNoLock(
    ULocalCharacterComponent* Owner,
    FLocalQwenWorkerState& WorkerState,
    const FString& RequestLine,
    FString& OutResponseLine,
    FString& OutErr,
    double TimeoutSeconds,
    const FThreadSafeBool* StopFlag,
    const FThreadSafeBool* InterruptedFlag)
{
    OutResponseLine.Reset();
    OutErr.Reset();
    FString IgnoredStdoutTail;

    const FString TimePrefix = LocalTalkerTimePrefix(Owner);
    const FString Speaker = Owner ? Owner->GetSpeakerNameResolved() : FString(TEXT("SharedQwen"));

    if (LocalTalkerShouldLogAudioTrace())
    {
        UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Qwen request -> %s"),
            *TimePrefix,
            *Speaker,
            *LocalTalkerPreview(RequestLine, 160));
    }

    if (!WorkerState.Handle.IsValid())
    {
        OutErr = TEXT("Qwen worker is not running.");
        return false;
    }
    if (!WorkerState.Pipes.WriteInPipe)
    {
        OutErr = TEXT("Qwen worker stdin pipe is unavailable.");
        return false;
    }

    if (!FLocalTalkerProcess::WriteStdin(WorkerState.Pipes, RequestLine))
    {
        OutErr = TEXT("Failed writing request to Qwen worker stdin.");
        return false;
    }

    const double Start = FPlatformTime::Seconds();
    const double Timeout = FMath::Max(1.0, TimeoutSeconds);

    while (true)
    {
        if ((StopFlag && !!(*StopFlag)) || (InterruptedFlag && !!(*InterruptedFlag)))
        {
            OutErr = TEXT("Qwen request canceled (TTS stop/interrupted).");
            return false;
        }

        const FString OutChunk = FLocalTalkerProcess::ReadAvailable(WorkerState.Pipes.ReadPipe);
        if (!OutChunk.IsEmpty())
        {
            WorkerState.StdoutBuffer += OutChunk;

            FString Line;
            while (LocalTalkerTryPopLine(WorkerState.StdoutBuffer, Line))
            {
                if (Line.IsEmpty())
                {
                    continue;
                }

                TSharedPtr<FJsonObject> Parsed;
                if (!LocalTalkerParseJsonLine(Line, Parsed) || !Parsed.IsValid())
                {
                    FString Snippet = Line;
                    Snippet.ReplaceInline(TEXT("\n"), TEXT(" "));
                    Snippet.ReplaceInline(TEXT("\r"), TEXT(" "));
                    Snippet.TrimStartAndEndInline();
                    if (Snippet.Len() > 200)
                    {
                        Snippet = Snippet.Left(200) + TEXT("...");
                    }

                    UE_LOG(LogLocalTalker, Verbose, TEXT("%s[%s] Qwen worker stdout (ignored non-JSON): %s"),
                        *TimePrefix,
                        *Speaker,
                        *Snippet);

                    if (!Snippet.IsEmpty())
                    {
                        if (!IgnoredStdoutTail.IsEmpty())
                        {
                            IgnoredStdoutTail += TEXT(" | ");
                        }
                        IgnoredStdoutTail += Snippet;
                        constexpr int32 MaxTailChars = 512;
                        if (IgnoredStdoutTail.Len() > MaxTailChars)
                        {
                            IgnoredStdoutTail = IgnoredStdoutTail.Right(MaxTailChars);
                        }
                    }
                    continue;
                }

                OutResponseLine = Line;
                if (LocalTalkerShouldLogAudioTrace())
                {
                    UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Qwen response <- %s"),
                        *TimePrefix,
                        *Speaker,
                        *LocalTalkerPreview(OutResponseLine, 200));
                }
                return true;
            }
        }

        const FString ErrChunk = FLocalTalkerProcess::ReadAvailable(WorkerState.Pipes.ReadErrPipe);
        if (!ErrChunk.IsEmpty())
        {
            TArray<FString> Lines;
            ErrChunk.ParseIntoArrayLines(Lines, /*CullEmpty*/ false);
            FString SignificantCombined;

            for (const FString& RawLine : Lines)
            {
                FString Line = RawLine;
                Line.TrimStartAndEndInline();
                if (Line.IsEmpty())
                {
                    continue;
                }

                if (LocalTalkerIsBenignQwenStderrLine(Line))
                {
                    bool bShouldLog = false;
                    {
                        FScopeLock SeenLock(&GLocalTalkerBenignStderrMutex);
                        if (!GLocalTalkerSeenBenignQwenStderr.Contains(Line))
                        {
                            GLocalTalkerSeenBenignQwenStderr.Add(Line);
                            bShouldLog = true;
                        }
                    }

                    if (bShouldLog && LocalTalkerShouldLogAudioTrace())
                    {
                        UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Qwen worker note (suppressed repeats): %s"),
                            *TimePrefix,
                            *Speaker,
                            *Line);
                    }
                    continue;
                }

                if (!SignificantCombined.IsEmpty())
                {
                    SignificantCombined += TEXT("\n");
                }
                SignificantCombined += RawLine;
            }

            if (!SignificantCombined.IsEmpty())
            {
                WorkerState.StderrBuffer += SignificantCombined;
                if (!WorkerState.StderrBuffer.EndsWith(TEXT("\n")))
                {
                    WorkerState.StderrBuffer += TEXT("\n");
                }
                UE_LOG(LogLocalTalker, Warning, TEXT("%s[%s] Qwen worker stderr: %s"),
                    *TimePrefix,
                    *Speaker,
                    *SignificantCombined);
            }
        }

        if (!FPlatformProcess::IsProcRunning(WorkerState.Handle))
        {
            OutErr = TEXT("Qwen worker exited unexpectedly.");
            if (!IgnoredStdoutTail.IsEmpty())
            {
                OutErr += TEXT(" stdout: ") + IgnoredStdoutTail;
            }
            if (!WorkerState.StderrBuffer.IsEmpty())
            {
                OutErr += TEXT(" stderr: ") + WorkerState.StderrBuffer;
            }
            return false;
        }

        if ((FPlatformTime::Seconds() - Start) >= Timeout)
        {
            OutErr = FString::Printf(TEXT("Timed out waiting for Qwen worker response (%.1fs)."), Timeout);
            if (!IgnoredStdoutTail.IsEmpty())
            {
                OutErr += TEXT(" stdout: ") + IgnoredStdoutTail.Right(256);
            }
            if (!WorkerState.StderrBuffer.IsEmpty())
            {
                OutErr += TEXT(" stderr: ") + WorkerState.StderrBuffer.Right(512);
            }
            return false;
        }

        FPlatformProcess::Sleep(0.005f);
    }
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

static bool LocalTalkerLooksMostlyAsciiEnglish(const FString& In)
{
    int32 AlphaCount = 0;
    int32 AsciiAlphaCount = 0;
    for (int32 i = 0; i < In.Len(); ++i)
    {
        const TCHAR C = In[i];
        if (FChar::IsAlpha(C))
        {
            ++AlphaCount;
            if (C >= 0 && C < 128)
            {
                ++AsciiAlphaCount;
            }
        }
    }

    if (AlphaCount <= 0)
    {
        return false;
    }

    return (AsciiAlphaCount * 100) >= (AlphaCount * 95);
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
    ShutdownQwenWorker();
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
    UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] LocalTalker paths: LlamaLib='%s' LlamaModel='%s' QwenPy='%s' QwenWorker='%s' QwenModel='%s' QwenTokenizer='%s' WorkDir='%s'"),
        *LocalTalkerTimePrefix(this),
        *GetSpeakerNameResolved(),
        *Paths.LlamaLibPath,
        *Paths.LlamaModelPath,
        *Paths.QwenPythonExePath,
        *Paths.QwenWorkerScriptPath,
        *Paths.QwenModelPath,
        *Paths.QwenTokenizerPath,
        *Paths.WorkingDir
    );

    const FString ResolvedMicDevice = GetResolvedMicInputDeviceName();
    UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Mic input device: %s"),
        *LocalTalkerTimePrefix(this),
        *GetSpeakerNameResolved(),
        ResolvedMicDevice.IsEmpty() ? TEXT("Default (System)") : *ResolvedMicDevice
    );

    KickoffQwenPrewarmIfNeeded(Paths);

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

    TryStartPendingAudio();
    if (AudioComp && !bAudioPlaybackComplete)
    {
        if (!AudioComp->IsPlaying() && !PendingAudioWave)
        {
            HandleAudioFinished();
        }
        else if (AudioComp->IsPlaying() && ActiveAudioStartWorldSeconds > 0.0 && ActiveAudioDurationSeconds > 0.0f)
        {
            if (UWorld* W = GetWorld())
            {
                const double NowSeconds = W->GetTimeSeconds();
                const double EndSeconds = ActiveAudioStartWorldSeconds + (double)ActiveAudioDurationSeconds + (double)FMath::Max(0.0f, AudioCompletionGraceSeconds);
                if (NowSeconds >= EndSeconds)
                {
                    AudioComp->Stop();
                    HandleAudioFinished();
                }
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
    if (bTraceAudio)
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
            const bool bHasSpeaker = !V.QwenSpeaker.IsEmpty();
            const bool bHasPrompt = !V.QwenVoicePromptPath.IsEmpty();
            if (bHasSpeaker || bHasPrompt)
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

bool ULocalCharacterComponent::ResolveQwenVoiceSelection(FString& OutSpeaker, FString& OutVoicePromptPath, FString& OutInstruction) const
{
    OutSpeaker.Reset();
    OutVoicePromptPath.Reset();
    OutInstruction.Reset();

    const ULocalTalkerSettings* S = GetDefault<ULocalTalkerSettings>();
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LocalTalker"));

    auto NormalizePromptPath = [&](FString& InOutPath)
    {
        if (InOutPath.IsEmpty() || !FPaths::IsRelative(InOutPath))
        {
            return;
        }

        if (Plugin.IsValid())
        {
            const FString Candidate = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/Voices"), InOutPath);
            if (FPaths::FileExists(Candidate))
            {
                InOutPath = Candidate;
                return;
            }
        }

        InOutPath = FPaths::ConvertRelativePathToFull(InOutPath);
    };

    auto ResolveFromSettingsId = [&](FName Id) -> bool
    {
        if (!S || Id.IsNone()) return false;
        for (const FLocalTalkVoiceOption& V : S->Voices)
        {
            if (V.Id != Id)
            {
                continue;
            }

            OutSpeaker = V.QwenSpeaker;
            OutInstruction = V.QwenInstruction;
            OutVoicePromptPath = V.QwenVoicePromptPath;
            NormalizePromptPath(OutVoicePromptPath);
            return true;
        }
        return false;
    };

    // 1) Component-selected ID from settings.
    if (!VoiceId.IsNone())
    {
        if (ResolveFromSettingsId(VoiceId))
        {
            return true;
        }

        // 2) Fallback: auto-discovered prompt asset by filename stem.
        if (Plugin.IsValid())
        {
            const FString P = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/Voices"), VoiceId.ToString() + TEXT(".voiceprompt.pt"));
            if (FPaths::FileExists(P))
            {
                OutVoicePromptPath = P;
                return true;
            }
        }
    }

    // 3) First configured voice in settings.
    if (S)
    {
        for (const FLocalTalkVoiceOption& V : S->Voices)
        {
            if (!V.Id.IsNone() && (!V.QwenSpeaker.IsEmpty() || !V.QwenVoicePromptPath.IsEmpty()))
            {
                OutSpeaker = V.QwenSpeaker;
                OutInstruction = V.QwenInstruction;
                OutVoicePromptPath = V.QwenVoicePromptPath;
                NormalizePromptPath(OutVoicePromptPath);
                return true;
            }
        }
    }

    return false;
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
    (void)TurnPrompt;

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
    FString LastLine;
    for (int32 i = StartIdx; i < ContextHistory.Num(); i++)
    {
        const FLocalTalkMessage& M = ContextHistory[i];
        if (M.bFromUser) continue;
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
    if (!PathsOverride.QwenPythonExePath.IsEmpty()) Out.QwenPythonExePath = PathsOverride.QwenPythonExePath;
    if (!PathsOverride.QwenWorkerScriptPath.IsEmpty()) Out.QwenWorkerScriptPath = PathsOverride.QwenWorkerScriptPath;
    if (!PathsOverride.QwenModelPath.IsEmpty()) Out.QwenModelPath = PathsOverride.QwenModelPath;
    if (!PathsOverride.QwenTokenizerPath.IsEmpty()) Out.QwenTokenizerPath = PathsOverride.QwenTokenizerPath;
    if (!PathsOverride.QwenDevice.IsEmpty()) Out.QwenDevice = PathsOverride.QwenDevice;
    if (!PathsOverride.QwenDType.IsEmpty()) Out.QwenDType = PathsOverride.QwenDType;
    if (!PathsOverride.QwenLanguage.IsEmpty()) Out.QwenLanguage = PathsOverride.QwenLanguage;
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

        if (Out.QwenWorkerScriptPath.IsEmpty())
        {
            Out.QwenWorkerScriptPath = FPaths::Combine(Base, TEXT("Resources/Qwen/qwen_tts_worker.py"));
        }

        if (Out.QwenModelPath.IsEmpty())
        {
            Out.QwenModelPath = TEXT("Qwen/Qwen3-TTS-12Hz-0.6B-CustomVoice");
        }

        if (Out.QwenTokenizerPath.IsEmpty())
        {
            Out.QwenTokenizerPath = TEXT("Qwen/Qwen3-TTS-Tokenizer-12Hz");
        }

        if (Out.QwenPythonExePath.IsEmpty())
        {
            Out.QwenPythonExePath = TEXT("python");
        }

        if (Out.QwenDevice.IsEmpty())
        {
            Out.QwenDevice = TEXT("cuda:0");
        }

        if (Out.QwenDType.IsEmpty())
        {
            Out.QwenDType = TEXT("bfloat16");
        }

        if (Out.QwenLanguage.IsEmpty())
        {
            Out.QwenLanguage = TEXT("English");
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
            Owner->RunQwenSentenceToAudio(Sentence, Paths, Err);
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
        UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Starting TTS worker (model='%s', tokenizer='%s', device='%s')."),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved(),
            *Paths.QwenModelPath,
            *Paths.QwenTokenizerPath,
            *Paths.QwenDevice);
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

    ShutdownQwenWorker();
}

bool ULocalCharacterComponent::EnsureQwenWorker(const FLocalTalkerRuntimePaths& Paths, FString& OutErr)
{
    OutErr.Reset();
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LocalTalker"));
    const FString PluginBase = Plugin.IsValid() ? Plugin->GetBaseDir() : FString();

    FString WorkerScriptPath = Paths.QwenWorkerScriptPath;
    if (FPaths::IsRelative(WorkerScriptPath))
    {
        const FString PluginCandidate = PluginBase.IsEmpty() ? FString() : FPaths::Combine(PluginBase, WorkerScriptPath);
        WorkerScriptPath = (!PluginCandidate.IsEmpty() && FPaths::FileExists(PluginCandidate))
            ? PluginCandidate
            : FPaths::ConvertRelativePathToFull(WorkerScriptPath);
    }
    if (WorkerScriptPath.IsEmpty() || !FPaths::FileExists(WorkerScriptPath))
    {
        OutErr = FString::Printf(TEXT("Qwen worker script not found: %s"), *WorkerScriptPath);
        return false;
    }
    if (Paths.QwenPythonExePath.IsEmpty())
    {
        OutErr = TEXT("QwenPythonExePath is empty. Configure Project Settings -> LocalTalker -> DefaultPaths.");
        return false;
    }
    if (Paths.QwenModelPath.IsEmpty())
    {
        OutErr = TEXT("QwenModelPath is empty. Configure Project Settings -> LocalTalker -> DefaultPaths.");
        return false;
    }

    bool bModelFromSnapshot = false;
    bool bTokenizerFromSnapshot = false;
    const FString ModelPath = LocalTalkerResolveQwenAssetPath(Paths.QwenModelPath, PluginBase, bModelFromSnapshot);
    FString TokenizerPath = LocalTalkerResolveQwenAssetPath(Paths.QwenTokenizerPath, PluginBase, bTokenizerFromSnapshot);
    if (LocalTalkerLooksLikeRepoId(TokenizerPath) && (FPaths::DirectoryExists(ModelPath) || FPaths::FileExists(ModelPath)))
    {
        // Qwen3TTSModel loads processor from the model path; prefer local model snapshot over remote tokenizer repo id.
        TokenizerPath = ModelPath;
    }

    const ULocalTalkerSettings* Settings = GetDefault<ULocalTalkerSettings>();
    const double Timeout = Settings ? FMath::Max(1.0, (double)Settings->QwenRequestTimeoutSeconds) : 180.0;
    const bool bUseFlashAttn = Settings && Settings->bQwenUseFlashAttention;
    const int32 WorkerPoolSize = Settings ? FMath::Clamp(Settings->QwenWorkerPoolSize, 1, 8) : 1;

    auto StartWorkerWithRuntime = [&](const FString& Device, const FString& DType, FString& OutStartErr) -> bool
    {
        OutStartErr.Reset();

        FScopeLock SharedLock(&GLocalTalkerSharedQwenWorkerMutex);
        const FString WorkerKey = LocalTalkerBuildQwenWorkerKey(
            Paths.QwenPythonExePath,
            WorkerScriptPath,
            ModelPath,
            TokenizerPath,
            Device,
            DType,
            Paths.WorkingDir,
            bUseFlashAttn,
            WorkerPoolSize);

        if (GLocalTalkerSharedQwenWorker.IsValid() &&
            GLocalTalkerSharedQwenWorker->WorkerKey == WorkerKey &&
            GLocalTalkerSharedQwenWorker->Workers.Num() == WorkerPoolSize)
        {
            bool bAllRunning = true;
            for (const TUniquePtr<FLocalQwenWorkerPoolEntry>& Entry : GLocalTalkerSharedQwenWorker->Workers)
            {
                if (!Entry.IsValid() ||
                    !Entry->Worker.Handle.IsValid() ||
                    !FPlatformProcess::IsProcRunning(Entry->Worker.Handle))
                {
                    bAllRunning = false;
                    break;
                }
            }
            if (bAllRunning)
            {
                QwenWorker = (GLocalTalkerSharedQwenWorker->Workers.Num() > 0 && GLocalTalkerSharedQwenWorker->Workers[0].IsValid())
                    ? &GLocalTalkerSharedQwenWorker->Workers[0]->Worker
                    : nullptr;
                return QwenWorker != nullptr;
            }
        }

        LocalTalkerShutdownSharedQwenWorkerNoLock();
        GLocalTalkerSharedQwenWorker = MakeUnique<FLocalTalkerSharedQwenWorkerState>();
        GLocalTalkerSharedQwenWorker->WorkerKey = WorkerKey;
        GLocalTalkerSharedQwenWorker->Workers.Reserve(WorkerPoolSize);

        for (int32 WorkerIndex = 0; WorkerIndex < WorkerPoolSize; ++WorkerIndex)
        {
            TUniquePtr<FLocalQwenWorkerPoolEntry> NewEntry = MakeUnique<FLocalQwenWorkerPoolEntry>();

            FString Args = QuoteArg3(WorkerScriptPath);
            Args += TEXT(" --model ") + QuoteArg3(ModelPath);
            if (!TokenizerPath.IsEmpty())
            {
                Args += TEXT(" --tokenizer ") + QuoteArg3(TokenizerPath);
            }
            if (!Device.IsEmpty())
            {
                Args += TEXT(" --device ") + QuoteArg3(Device);
            }
            if (!DType.IsEmpty())
            {
                Args += TEXT(" --dtype ") + QuoteArg3(DType);
            }
            Args += bUseFlashAttn ? TEXT(" --flash-attn") : TEXT(" --no-flash-attn");

            if (LocalTalkerShouldLogAudioTrace())
            {
                UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Qwen worker spawn cfg[%d/%d]: python='%s' script='%s' cwd='%s' device='%s' dtype='%s' flash=%d"),
                    *LocalTalkerTimePrefix(this),
                    *GetSpeakerNameResolved(),
                    WorkerIndex + 1,
                    WorkerPoolSize,
                    *Paths.QwenPythonExePath,
                    *WorkerScriptPath,
                    *Paths.WorkingDir,
                    *Device,
                    *DType,
                    bUseFlashAttn ? 1 : 0);
            }

            FString SpawnErr;
            if (!FLocalTalkerProcess::SpawnWithPipes(
                Paths.QwenPythonExePath,
                Args,
                Paths.WorkingDir,
                NewEntry->Worker.Handle,
                NewEntry->Worker.Pipes,
                SpawnErr))
            {
                OutStartErr = FString::Printf(TEXT("Failed to start Qwen worker %d/%d: %s"), WorkerIndex + 1, WorkerPoolSize, *SpawnErr);
                LocalTalkerShutdownSharedQwenWorkerNoLock();
                return false;
            }

            TSharedRef<FJsonObject> HealthReq = MakeShared<FJsonObject>();
            HealthReq->SetStringField(TEXT("cmd"), TEXT("health"));
            FString HealthLine;
            if (!LocalTalkerSerializeJsonLine(HealthReq, HealthLine))
            {
                OutStartErr = TEXT("Failed to serialize Qwen health request.");
                LocalTalkerShutdownSharedQwenWorkerNoLock();
                return false;
            }

            FString HealthRespLine;
            if (!LocalTalkerSendQwenWorkerRequestNoLock(
                this,
                NewEntry->Worker,
                HealthLine,
                HealthRespLine,
                OutStartErr,
                Timeout,
                &bTTSStop,
                &bInterrupted))
            {
                LocalTalkerShutdownSharedQwenWorkerNoLock();
                return false;
            }

            TSharedPtr<FJsonObject> HealthResp;
            if (!LocalTalkerParseJsonLine(HealthRespLine, HealthResp) || !HealthResp.IsValid())
            {
                OutStartErr = FString::Printf(TEXT("Invalid health response from Qwen worker %d/%d: %s"), WorkerIndex + 1, WorkerPoolSize, *HealthRespLine);
                LocalTalkerShutdownSharedQwenWorkerNoLock();
                return false;
            }

            bool bOk = false;
            if (!HealthResp->TryGetBoolField(TEXT("ok"), bOk) || !bOk)
            {
                FString WorkerErr;
                HealthResp->TryGetStringField(TEXT("error"), WorkerErr);
                OutStartErr = FString::Printf(TEXT("Qwen worker %d/%d health check failed: %s"), WorkerIndex + 1, WorkerPoolSize, *WorkerErr);
                LocalTalkerShutdownSharedQwenWorkerNoLock();
                return false;
            }

            if (LocalTalkerShouldLogAudioTrace())
            {
                FString PyExe;
                FString PyVer;
                FString TorchVer;
                FString TorchCudaVer;
                FString CudaDevName;
                FString DeviceResolved;
                FString DTypeResolved;
                double PidNum = 0.0;
                double CudaDeviceCountNum = 0.0;
                bool bCudaAvail = false;
                bool bFlash = false;
                HealthResp->TryGetStringField(TEXT("python_executable"), PyExe);
                HealthResp->TryGetStringField(TEXT("python_version"), PyVer);
                HealthResp->TryGetStringField(TEXT("torch_version"), TorchVer);
                HealthResp->TryGetStringField(TEXT("torch_cuda_version"), TorchCudaVer);
                HealthResp->TryGetStringField(TEXT("cuda_device_name"), CudaDevName);
                HealthResp->TryGetStringField(TEXT("device_resolved"), DeviceResolved);
                HealthResp->TryGetStringField(TEXT("dtype_resolved"), DTypeResolved);
                HealthResp->TryGetNumberField(TEXT("pid"), PidNum);
                HealthResp->TryGetNumberField(TEXT("cuda_device_count"), CudaDeviceCountNum);
                HealthResp->TryGetBoolField(TEXT("cuda_available"), bCudaAvail);
                HealthResp->TryGetBoolField(TEXT("flash_attn"), bFlash);
                const int32 Pid = (int32)PidNum;
                const int32 CudaDeviceCount = (int32)CudaDeviceCountNum;

                UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Qwen health[%d/%d]: pid=%d py='%s' pyVer='%s' torch='%s' cu='%s' cudaAvail=%d cudaDevs=%d cudaDev0='%s' resolvedDevice='%s' resolvedDType='%s' flash=%d"),
                    *LocalTalkerTimePrefix(this),
                    *GetSpeakerNameResolved(),
                    WorkerIndex + 1,
                    WorkerPoolSize,
                    Pid,
                    *PyExe,
                    *PyVer,
                    *TorchVer,
                    *TorchCudaVer,
                    bCudaAvail ? 1 : 0,
                    CudaDeviceCount,
                    *CudaDevName,
                    *DeviceResolved,
                    *DTypeResolved,
                    bFlash ? 1 : 0);
            }

            GLocalTalkerSharedQwenWorker->Workers.Add(MoveTemp(NewEntry));
        }

        QwenWorker = (GLocalTalkerSharedQwenWorker->Workers.Num() > 0 && GLocalTalkerSharedQwenWorker->Workers[0].IsValid())
            ? &GLocalTalkerSharedQwenWorker->Workers[0]->Worker
            : nullptr;

        if (bModelFromSnapshot || bTokenizerFromSnapshot)
        {
            UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Qwen local snapshot resolved: model='%s' tokenizer='%s'."),
                *LocalTalkerTimePrefix(this),
                *GetSpeakerNameResolved(),
                *ModelPath,
                *TokenizerPath);
        }

        UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Qwen worker ready (shared=%d device='%s', dtype='%s')."),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved(),
            WorkerPoolSize,
            *Device,
            *DType);
        return QwenWorker != nullptr;
    };

    const FString RequestedDevice = Paths.QwenDevice;
    const FString RequestedDType = Paths.QwenDType;
    if (StartWorkerWithRuntime(RequestedDevice, RequestedDType, OutErr))
    {
        return true;
    }

    const bool bRequestedCpu = RequestedDevice.Equals(TEXT("cpu"), ESearchCase::IgnoreCase);
    const bool bGpuInitFailure =
        OutErr.Contains(TEXT("cuda"), ESearchCase::IgnoreCase) ||
        OutErr.Contains(TEXT("CUDA"), ESearchCase::IgnoreCase) ||
        OutErr.Contains(TEXT("Torch not compiled with CUDA enabled"), ESearchCase::IgnoreCase) ||
        OutErr.Contains(TEXT("not compiled with CUDA"), ESearchCase::IgnoreCase) ||
        OutErr.Contains(TEXT("device"), ESearchCase::IgnoreCase);

    if (!bRequestedCpu && bGpuInitFailure)
    {
        const FString PrimaryErr = OutErr;
        UE_LOG(LogLocalTalker, Warning, TEXT("%s[%s] Qwen worker init failed on device '%s'. Retrying with CPU/float32. Error: %s"),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved(),
            *RequestedDevice,
            *PrimaryErr);

        FString CpuErr;
        if (StartWorkerWithRuntime(TEXT("cpu"), TEXT("float32"), CpuErr))
        {
            return true;
        }

        OutErr = FString::Printf(TEXT("Qwen worker failed on '%s' and CPU fallback. primary='%s' fallback='%s'"),
            *RequestedDevice,
            *PrimaryErr,
            *CpuErr);
        return false;
    }

    return false;
}

bool ULocalCharacterComponent::SendQwenWorkerRequest(const FString& RequestLine, FString& OutResponseLine, FString& OutErr, double TimeoutSeconds)
{
    FLocalQwenWorkerPoolEntry* SelectedEntry = nullptr;
    {
        FScopeLock SharedLock(&GLocalTalkerSharedQwenWorkerMutex);
        if (GLocalTalkerSharedQwenWorker.IsValid() && GLocalTalkerSharedQwenWorker->Workers.Num() > 0)
        {
            int32 BestInFlight = MAX_int32;
            for (const TUniquePtr<FLocalQwenWorkerPoolEntry>& Entry : GLocalTalkerSharedQwenWorker->Workers)
            {
                if (!Entry.IsValid() ||
                    !Entry->Worker.Handle.IsValid() ||
                    !FPlatformProcess::IsProcRunning(Entry->Worker.Handle))
                {
                    continue;
                }

                const int32 InFlight = Entry->InFlightRequests.GetValue();
                if (!SelectedEntry || InFlight < BestInFlight)
                {
                    SelectedEntry = Entry.Get();
                    BestInFlight = InFlight;
                }
            }
        }
    }

    if (!SelectedEntry)
    {
        OutErr = TEXT("Qwen worker is not running.");
        return false;
    }

    SelectedEntry->InFlightRequests.Increment();
    const auto DecrementInFlight = [SelectedEntry]()
    {
        SelectedEntry->InFlightRequests.Decrement();
    };

    bool bOk = false;
    {
        FScopeLock RequestLock(&SelectedEntry->RequestMutex);
        bOk = LocalTalkerSendQwenWorkerRequestNoLock(
            this,
            SelectedEntry->Worker,
            RequestLine,
            OutResponseLine,
            OutErr,
            TimeoutSeconds,
            &bTTSStop,
            &bInterrupted);
    }
    DecrementInFlight();
    return bOk;
}

void ULocalCharacterComponent::ShutdownQwenWorker()
{
    QwenWorker = nullptr;
}

void ULocalCharacterComponent::ShutdownSharedQwenWorkerGlobal()
{
    FScopeLock SharedLock(&GLocalTalkerSharedQwenWorkerMutex);
    LocalTalkerShutdownSharedQwenWorkerNoLock();
}

void ULocalCharacterComponent::KickoffQwenPrewarmIfNeeded(const FLocalTalkerRuntimePaths& Paths)
{
    bool bShouldStart = false;
    {
        FScopeLock SharedLock(&GLocalTalkerSharedQwenWorkerMutex);
        if (!GLocalTalkerQwenPrewarmStarted)
        {
            GLocalTalkerQwenPrewarmStarted = true;
            bShouldStart = true;
        }
    }

    if (!bShouldStart)
    {
        return;
    }

    const TWeakObjectPtr<ULocalCharacterComponent> WeakThis(this);
    Async(EAsyncExecution::ThreadPool, [WeakThis, Paths]()
    {
        ULocalCharacterComponent* Owner = WeakThis.Get();
        if (!Owner)
        {
            return;
        }

        FString EnsureErr;
        if (!Owner->EnsureQwenWorker(Paths, EnsureErr))
        {
            UE_LOG(LogLocalTalker, Warning, TEXT("%s[%s] Qwen prewarm failed during worker init: %s"),
                *LocalTalkerTimePrefix(Owner),
                *Owner->GetSpeakerNameResolved(),
                *EnsureErr);
            FScopeLock SharedLock(&GLocalTalkerSharedQwenWorkerMutex);
            GLocalTalkerQwenPrewarmStarted = false;
            return;
        }

        // Run one tiny synthesis to pay first-run model graph/kernel costs outside gameplay turn timing.
        const FString SavedDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
        const FString PrewarmWav = FPaths::Combine(SavedDir, TEXT("LocalTalker"), TEXT("qwen_prewarm_ready.wav"));
        TSharedRef<FJsonObject> Req = MakeShared<FJsonObject>();
        Req->SetStringField(TEXT("cmd"), TEXT("synthesize"));
        Req->SetStringField(TEXT("text"), TEXT("Ready."));
        Req->SetStringField(TEXT("language"), TEXT("English"));
        Req->SetStringField(TEXT("output_wav"), PrewarmWav);
        Req->SetStringField(TEXT("speaker"), LocalTalkerGetDefaultQwenSpeaker());
        Req->SetBoolField(TEXT("non_streaming_mode"), true);
        Req->SetBoolField(TEXT("do_sample"), false);
        Req->SetNumberField(TEXT("max_new_tokens"), 128);
        Req->SetStringField(TEXT("request_id"), TEXT("prewarm-ready"));

        FString ReqLine;
        if (!LocalTalkerSerializeJsonLine(Req, ReqLine))
        {
            UE_LOG(LogLocalTalker, Warning, TEXT("%s[%s] Qwen prewarm synth skipped: failed to serialize request."),
                *LocalTalkerTimePrefix(Owner),
                *Owner->GetSpeakerNameResolved());
            UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Qwen prewarm complete (worker init only)."),
                *LocalTalkerTimePrefix(Owner),
                *Owner->GetSpeakerNameResolved());
            return;
        }

        const ULocalTalkerSettings* Settings = GetDefault<ULocalTalkerSettings>();
        const double Timeout = Settings ? FMath::Max(1.0, (double)Settings->QwenRequestTimeoutSeconds) : 180.0;
        FString RespLine;
        FString ReqErr;
        if (!Owner->SendQwenWorkerRequest(ReqLine, RespLine, ReqErr, Timeout))
        {
            UE_LOG(LogLocalTalker, Warning, TEXT("%s[%s] Qwen prewarm synth failed: %s"),
                *LocalTalkerTimePrefix(Owner),
                *Owner->GetSpeakerNameResolved(),
                *ReqErr);
            UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Qwen prewarm complete (worker init only)."),
                *LocalTalkerTimePrefix(Owner),
                *Owner->GetSpeakerNameResolved());
            return;
        }

        TSharedPtr<FJsonObject> Resp;
        if (!LocalTalkerParseJsonLine(RespLine, Resp) || !Resp.IsValid())
        {
            UE_LOG(LogLocalTalker, Warning, TEXT("%s[%s] Qwen prewarm synth returned invalid JSON."),
                *LocalTalkerTimePrefix(Owner),
                *Owner->GetSpeakerNameResolved());
            UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Qwen prewarm complete (worker init only)."),
                *LocalTalkerTimePrefix(Owner),
                *Owner->GetSpeakerNameResolved());
            return;
        }

        bool bOk = false;
        Resp->TryGetBoolField(TEXT("ok"), bOk);
        const TSharedPtr<FJsonObject>* MetricsObj = nullptr;
        double ModelSeconds = 0.0;
        double TotalSeconds = 0.0;
        if (Resp->TryGetObjectField(TEXT("metrics"), MetricsObj) && MetricsObj && MetricsObj->IsValid())
        {
            (*MetricsObj)->TryGetNumberField(TEXT("model_seconds"), ModelSeconds);
            (*MetricsObj)->TryGetNumberField(TEXT("total_seconds"), TotalSeconds);
        }

        IFileManager::Get().Delete(*PrewarmWav);

        UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Qwen prewarm complete (worker + synth). ok=%d model=%.2fs total=%.2fs"),
            *LocalTalkerTimePrefix(Owner),
            *Owner->GetSpeakerNameResolved(),
            bOk ? 1 : 0,
            ModelSeconds,
            TotalSeconds);
    });
}

void ULocalCharacterComponent::RunQwenSentenceToAudio(const FString& Sentence, const FLocalTalkerRuntimePaths& Paths, FString& OutErr)
{
    if (LocalTalkerShouldLogAudioTrace())
    {
        UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] RunQwenSentenceToAudio begin (%d chars): %s"),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved(),
            Sentence.Len(),
            *LocalTalkerPreview(Sentence));
    }

    TArray<uint8> Bytes;
    int32 SampleRate = 0;
    int32 NumChannels = 0;

    if (!GenerateQwenAudioBytes(Sentence, Paths, Bytes, SampleRate, NumChannels, OutErr))
    {
        UE_LOG(LogLocalTalker, Error, TEXT("%s[%s] RunQwenSentenceToAudio failed: %s"),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved(),
            *OutErr);
        return;
    }

    const float DurationSec = (SampleRate > 0 && NumChannels > 0)
        ? ((float)Bytes.Num() / (float)(2 * NumChannels * SampleRate))
        : 0.0f;

    const int32 QueuedBytes = Bytes.Num();
    const FString SentenceCopy = Sentence;

    AsyncTask(ENamedThreads::GameThread, [this, Bytes = MoveTemp(Bytes), SampleRate, NumChannels, DurationSec, SentenceCopy]() mutable
    {
        EnsureAudio();
        if (!AudioComp)
        {
            UE_LOG(LogLocalTalker, Error, TEXT("%s[%s] Audio enqueue aborted: AudioComp is null."),
                *LocalTalkerTimePrefix(this),
                *GetSpeakerNameResolved());
            return;
        }

        USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(this, TEXT("LocalTalkerProcWave"));
        Wave->bLooping = false;
        Wave->Duration = INDEFINITELY_LOOPING_DURATION;
        Wave->SampleByteSize = sizeof(int16);
        Wave->NumChannels = NumChannels;
        Wave->SetSampleRate(SampleRate);
        if (Bytes.Num() > 0)
        {
            Wave->QueueAudio(Bytes.GetData(), Bytes.Num());
        }

        PendingAudioWave = Wave;
        PendingSubtitleText = SentenceCopy;
        PendingSubtitleDurationSeconds = DurationSec;
        bAudioPlaybackComplete = false;

        if (LocalTalkerShouldLogAudioTrace())
        {
            UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Pending audio prepared: bytes=%d rate=%d channels=%d duration=%.2fs"),
                *LocalTalkerTimePrefix(this),
                *GetSpeakerNameResolved(),
                Bytes.Num(),
                SampleRate,
                NumChannels,
                DurationSec);
        }

        TryStartPendingAudio();
    });

    UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Qwen TTS ok: %d bytes, %d ch, %d Hz (ready)"),
        *LocalTalkerTimePrefix(this),
        *GetSpeakerNameResolved(),
        QueuedBytes,
        NumChannels,
        SampleRate
    );
}

bool ULocalCharacterComponent::GenerateQwenAudioBytes(const FString& Sentence, const FLocalTalkerRuntimePaths& Paths, TArray<uint8>& OutBytes, int32& OutSampleRate, int32& OutNumChannels, FString& OutErr)
{
    const ULocalTalkerSettings* Settings = GetDefault<ULocalTalkerSettings>();
    if (Settings && Settings->TtsBackend == ELocalTalkTtsBackend::PiperLegacy)
    {
        static bool bWarnedLegacyBackend = false;
        if (!bWarnedLegacyBackend)
        {
            bWarnedLegacyBackend = true;
            UE_LOG(LogLocalTalker, Warning,
                TEXT("%s[%s] TTS backend is set to PiperLegacy, but this runtime is Qwen-only. Falling back to Qwen worker."),
                *LocalTalkerTimePrefix(this),
                *GetSpeakerNameResolved());
        }
    }

    if (Sentence.IsEmpty())
    {
        OutErr = TEXT("Cannot synthesize empty sentence.");
        return false;
    }

    if (!EnsureQwenWorker(Paths, OutErr))
    {
        UE_LOG(LogLocalTalker, Error, TEXT("%s[%s] EnsureQwenWorker failed: %s"),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved(),
            *OutErr);
        return false;
    }

    UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Qwen TTS start: %s"), *LocalTalkerTimePrefix(this), *GetSpeakerNameResolved(), *Sentence);

    const FString TempDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("LocalTalker"));
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*TempDir);

    const FString OutWav = FPaths::Combine(TempDir, FString::Printf(TEXT("qwen_tts_%llu.wav"), (uint64)FPlatformTime::Cycles64()));
    FString Speaker;
    FString VoicePromptPath;
    FString VoiceInstruction;
    ResolveQwenVoiceSelection(Speaker, VoicePromptPath, VoiceInstruction);
    if (Speaker.IsEmpty())
    {
        Speaker = LocalTalkerGetDefaultQwenSpeaker();
        UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Qwen speaker fallback applied: '%s'."),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved(),
            *Speaker);
    }

    if (LocalTalkerShouldLogAudioTrace())
    {
        UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Qwen voice resolved: speaker='%s' instructChars=%d prompt='%s'"),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved(),
            *Speaker,
            VoiceInstruction.Len(),
            *VoicePromptPath);
    }

    if (FPaths::IsRelative(VoicePromptPath) && !VoicePromptPath.IsEmpty())
    {
        VoicePromptPath = FPaths::ConvertRelativePathToFull(VoicePromptPath);
    }

    FString RequestLanguage = Paths.QwenLanguage.IsEmpty() ? TEXT("English") : Paths.QwenLanguage;
    if (RequestLanguage.Equals(TEXT("Auto"), ESearchCase::IgnoreCase) && LocalTalkerLooksMostlyAsciiEnglish(Sentence))
    {
        RequestLanguage = TEXT("English");
    }

    TSharedRef<FJsonObject> Req = MakeShared<FJsonObject>();
    Req->SetStringField(TEXT("cmd"), TEXT("synthesize"));
    Req->SetStringField(TEXT("text"), Sentence);
    Req->SetStringField(TEXT("language"), RequestLanguage);
    Req->SetStringField(TEXT("output_wav"), OutWav);
    Req->SetBoolField(TEXT("non_streaming_mode"), true);
    const FString QwenRequestId = FString::Printf(TEXT("%s-%llu"),
        *GetSpeakerNameResolved(),
        (uint64)FPlatformTime::Cycles64());
    Req->SetStringField(TEXT("request_id"), QwenRequestId);
    const int32 ConfiguredMaxNewTokens = Settings ? FMath::Max(0, Settings->QwenMaxNewTokens) : 0;
    const int32 AdaptiveMaxNewTokens = FMath::Clamp(96 + Sentence.Len() * 2, 128, 384);
    const int32 EffectiveMaxNewTokens = (ConfiguredMaxNewTokens > 0)
        ? FMath::Min(ConfiguredMaxNewTokens, AdaptiveMaxNewTokens)
        : AdaptiveMaxNewTokens;
    if (Settings)
    {
        Req->SetBoolField(TEXT("do_sample"), Settings->bQwenDoSample);
    }
    Req->SetNumberField(TEXT("max_new_tokens"), EffectiveMaxNewTokens);
    if (!Speaker.IsEmpty())
    {
        Req->SetStringField(TEXT("speaker"), Speaker);
    }
    if (!VoiceInstruction.IsEmpty())
    {
        Req->SetStringField(TEXT("instruct"), VoiceInstruction);
    }
    if (!VoicePromptPath.IsEmpty())
    {
        Req->SetStringField(TEXT("voice_prompt_path"), VoicePromptPath);
    }

    const int32 RequestMaxNewTokens = EffectiveMaxNewTokens;
    const bool bRequestDoSample = Settings ? Settings->bQwenDoSample : false;
    UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Qwen synth opts: req_id=%s do_sample=%d max_new_tokens=%d (configured=%d adaptive=%d)"),
        *LocalTalkerTimePrefix(this),
        *GetSpeakerNameResolved(),
        *QwenRequestId,
        bRequestDoSample ? 1 : 0,
        RequestMaxNewTokens,
        ConfiguredMaxNewTokens,
        AdaptiveMaxNewTokens);

    FString ReqLine;
    if (!LocalTalkerSerializeJsonLine(Req, ReqLine))
    {
        OutErr = TEXT("Failed to serialize Qwen synthesis request.");
        return false;
    }

    const double Timeout = Settings ? FMath::Max(1.0, (double)Settings->QwenRequestTimeoutSeconds) : 180.0;
    FString RespLine;
    if (!SendQwenWorkerRequest(ReqLine, RespLine, OutErr, Timeout))
    {
        UE_LOG(LogLocalTalker, Error, TEXT("%s[%s] Qwen synth request failed: %s"),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved(),
            *OutErr);
        return false;
    }

    TSharedPtr<FJsonObject> Resp;
    if (!LocalTalkerParseJsonLine(RespLine, Resp) || !Resp.IsValid())
    {
        OutErr = FString::Printf(TEXT("Invalid Qwen synthesis response: %s"), *RespLine);
        UE_LOG(LogLocalTalker, Error, TEXT("%s[%s] %s"),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved(),
            *OutErr);
        return false;
    }

    bool bOk = false;
    Resp->TryGetBoolField(TEXT("ok"), bOk);
    if (!bOk)
    {
        FString WorkerErr;
        Resp->TryGetStringField(TEXT("error"), WorkerErr);

        // Compatibility fallback: if a stale speaker id is configured, retry once with no explicit speaker.
        const bool bHasRequestedSpeaker = Req->HasField(TEXT("speaker"));
        if (bHasRequestedSpeaker && WorkerErr.Contains(TEXT("Unsupported speakers"), ESearchCase::IgnoreCase))
        {
            Req->RemoveField(TEXT("speaker"));

            FString RetryReqLine;
            if (!LocalTalkerSerializeJsonLine(Req, RetryReqLine))
            {
                OutErr = TEXT("Failed to serialize fallback Qwen synthesis request.");
                return false;
            }

            UE_LOG(LogLocalTalker, Warning, TEXT("%s[%s] Qwen speaker was unsupported; retrying synthesis with model default speaker."),
                *LocalTalkerTimePrefix(this),
                *GetSpeakerNameResolved());

            FString RetryRespLine;
            if (!SendQwenWorkerRequest(RetryReqLine, RetryRespLine, OutErr, Timeout))
            {
                UE_LOG(LogLocalTalker, Error, TEXT("%s[%s] Qwen synth fallback request failed: %s"),
                    *LocalTalkerTimePrefix(this),
                    *GetSpeakerNameResolved(),
                    *OutErr);
                return false;
            }

            if (!LocalTalkerParseJsonLine(RetryRespLine, Resp) || !Resp.IsValid())
            {
                OutErr = FString::Printf(TEXT("Invalid Qwen fallback synthesis response: %s"), *RetryRespLine);
                UE_LOG(LogLocalTalker, Error, TEXT("%s[%s] %s"),
                    *LocalTalkerTimePrefix(this),
                    *GetSpeakerNameResolved(),
                    *OutErr);
                return false;
            }

            Resp->TryGetBoolField(TEXT("ok"), bOk);
            if (!bOk)
            {
                WorkerErr.Reset();
                Resp->TryGetStringField(TEXT("error"), WorkerErr);
                OutErr = FString::Printf(TEXT("Qwen synthesis failed (including fallback): %s"), *WorkerErr);
                UE_LOG(LogLocalTalker, Error, TEXT("%s[%s] %s"),
                    *LocalTalkerTimePrefix(this),
                    *GetSpeakerNameResolved(),
                    *OutErr);
                return false;
            }
        }
        else
        {
            OutErr = FString::Printf(TEXT("Qwen synthesis failed: %s"), *WorkerErr);
            UE_LOG(LogLocalTalker, Error, TEXT("%s[%s] %s"),
                *LocalTalkerTimePrefix(this),
                *GetSpeakerNameResolved(),
                *OutErr);
            return false;
        }
    }

    FString WavPath = OutWav;
    Resp->TryGetStringField(TEXT("wav_path"), WavPath);
    if (WavPath.IsEmpty())
    {
        OutErr = TEXT("Qwen worker returned no wav_path.");
        UE_LOG(LogLocalTalker, Error, TEXT("%s[%s] %s"),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved(),
            *OutErr);
        return false;
    }

    if (LocalTalkerShouldLogAudioTrace())
    {
        UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Qwen synth response wav_path='%s'"),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved(),
            *WavPath);
    }

    const TSharedPtr<FJsonObject>* MetricsObj = nullptr;
    if (Resp->TryGetObjectField(TEXT("metrics"), MetricsObj) && MetricsObj && MetricsObj->IsValid())
    {
        double ModelSeconds = 0.0;
        double WriteSeconds = 0.0;
        double TotalSeconds = 0.0;
        bool bCacheHit = false;
        FString RespReqId;
        (*MetricsObj)->TryGetNumberField(TEXT("model_seconds"), ModelSeconds);
        (*MetricsObj)->TryGetNumberField(TEXT("write_seconds"), WriteSeconds);
        (*MetricsObj)->TryGetNumberField(TEXT("total_seconds"), TotalSeconds);
        (*MetricsObj)->TryGetBoolField(TEXT("cache_hit"), bCacheHit);
        (*MetricsObj)->TryGetStringField(TEXT("request_id"), RespReqId);

        UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Qwen metrics: req_id='%s' cacheHit=%d model=%.2fs write=%.3fs total=%.2fs"),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved(),
            *RespReqId,
            bCacheHit ? 1 : 0,
            ModelSeconds,
            WriteSeconds,
            TotalSeconds);
    }

    FLocalWavPcm16 W;
    FString WavErr;
    if (!FLocalTalkerWav::LoadWavPcm16(WavPath, W, WavErr))
    {
        OutErr = WavErr;
        UE_LOG(LogLocalTalker, Error, TEXT("%s[%s] Qwen WAV load failed: %s"), *LocalTalkerTimePrefix(this), *GetSpeakerNameResolved(), *OutErr);
        return false;
    }

    if (W.Samples.Num() == 0)
    {
        OutErr = TEXT("Qwen worker produced an empty WAV.");
        UE_LOG(LogLocalTalker, Error, TEXT("%s[%s] %s"),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved(),
            *OutErr);
        return false;
    }

    // Clean up the temp file ASAP; we have the audio in memory now.
    PF.DeleteFile(*WavPath);

    // Qwen output should be mono, but handle stereo->mono defensively.
    int32 NumChannels = FMath::Max(1, W.NumChannels);
    const int32 SampleRate = FMath::Max(1, W.SampleRate);

    TArray<int16> Mono;
    const int32 TotalSamples = W.Samples.Num();
    if (NumChannels == 2)
    {
        const int32 Frames = TotalSamples / 2;
        Mono.SetNumUninitialized(Frames);
        for (int32 i = 0; i < Frames; i++)
        {
            const int32 L = (int32)W.Samples[i * 2 + 0];
            const int32 R = (int32)W.Samples[i * 2 + 1];
            Mono[i] = (int16)((L + R) / 2);
        }
        NumChannels = 1;
    }
    else if (NumChannels != 1)
    {
        OutErr = FString::Printf(TEXT("Unsupported WAV channel count: %d (only mono/stereo supported)."), NumChannels);
        return false;
    }

    const TArray<int16>& Use = (Mono.Num() > 0) ? Mono : W.Samples;
    OutBytes.SetNumUninitialized(Use.Num() * sizeof(int16));
    FMemory::Memcpy(OutBytes.GetData(), Use.GetData(), OutBytes.Num());

    OutSampleRate = SampleRate;
    OutNumChannels = NumChannels;
    if (LocalTalkerShouldLogAudioTrace())
    {
        UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] WAV decode ok: samples=%d bytes=%d rate=%d channels=%d"),
            *LocalTalkerTimePrefix(this),
            *GetSpeakerNameResolved(),
            Use.Num(),
            OutBytes.Num(),
            OutSampleRate,
            OutNumChannels);
    }
    return true;
}

void ULocalCharacterComponent::TryStartPendingAudio()
{
    const bool bTraceAudio = LocalTalkerShouldLogAudioTrace();
    static double NextBlockedLogAt = 0.0;

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
        if (bTraceAudio && FPlatformTime::Seconds() >= NextBlockedLogAt)
        {
            NextBlockedLogAt = FPlatformTime::Seconds() + 0.5;
            UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] TryStartPendingAudio waiting: blocked by other active speaker."),
                *LocalTalkerTimePrefix(this),
                *GetSpeakerNameResolved());
        }
        return;
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
