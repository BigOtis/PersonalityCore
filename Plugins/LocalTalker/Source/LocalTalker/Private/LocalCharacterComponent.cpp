#include "LocalCharacterComponent.h"
#include "LocalTalkerSettings.h"
#include "LocalTalkerInProcAsync.h"
#include "LocalTalkerProcess.h"
#include "LocalTalkerWav.h"
#include "LocalTalkerLog.h"
#include "LocalTalkConversationSubsystem.h"

#include "Async/Async.h"
#include "Engine/Engine.h"
#include "SubtitleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Misc/PathViews.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"

static FString QuoteArg3(const FString& S)
{
    FString T = S;
    T.ReplaceInline(TEXT("\""), TEXT("\\\""));
    return FString::Printf(TEXT("\"%s\""), *T);
}

static bool IsSentenceTerminator(TCHAR C)
{
    return C == TEXT('.') || C == TEXT('!') || C == TEXT('?') || C == TEXT('\n');
}

static FString TrimSentence(const FString& In)
{
    FString S = In;
    S.ReplaceInline(TEXT("\r"), TEXT(""));
    S.TrimStartAndEndInline();
    return S;
}

static double Cycles64ToSecondsSafe(int64 Cycles)
{
    if (Cycles <= 0) return 0.0;
    return FPlatformTime::ToSeconds64((uint64)Cycles);
}

ULocalCharacterComponent::ULocalCharacterComponent()
{
    PrimaryComponentTick.bCanEverTick = true;

    // Sensible prompt defaults (editable on the component).
    DirectionsPrompt =
        TEXT("You are a helpful game character in Unreal Engine.\n")
        TEXT("Follow the player's instructions carefully.\n")
        TEXT("If you are unsure, ask a short clarifying question.\n")
        TEXT("Keep responses concise and actionable.\n")
        TEXT("Do not mention being an AI or a language model.\n");

    CharacterDescriptionPrompt = TEXT("");

    // Prefer real UE subtitles instead of debug prints.
    bUseUESubtitles = true;
    bShowOnScreenSubtitles = false;
}

void ULocalCharacterComponent::BeginPlay()
{
    Super::BeginPlay();
    EnsureAudio();

    // Always register so the conversation subsystem can prevent overlap for *any* LocalTalk speaker.
    if (UWorld* W = GetWorld())
    {
        if (ULocalTalkConversationSubsystem* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            Sub->RegisterTalker(this);
        }
    }

    // Quick visibility into what the component is using at runtime.
    const FLocalTalkerRuntimePaths Paths = ResolvePaths();
    UE_LOG(LogLocalTalker, Log, TEXT("[%s] LocalTalker paths: LlamaLib='%s' Model='%s' PiperExe='%s' Voice='%s' WorkDir='%s'"),
        *GetSpeakerNameResolved(),
        *Paths.LlamaLibPath, *Paths.LlamaModelPath, *Paths.PiperExePath, *Paths.PiperVoiceModelPath, *Paths.WorkingDir
    );
}

void ULocalCharacterComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Interrupt();
    StopTTSWorker();

    if (UWorld* W = GetWorld())
    {
        if (ULocalTalkConversationSubsystem* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
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

    // Backpressure: don't enqueue more sentences if TTS/audio is already backed up.
    const int32 PendingSentences = PendingSentenceCount.GetValue();
    const int32 PendingAudioChunks = PendingAudioChunkCount.GetValue();
    const bool bBackpressured =
        (MaxQueuedSentencesAhead > 0 && PendingSentences >= MaxQueuedSentencesAhead) ||
        (MaxQueuedAudioChunksAhead > 0 && PendingAudioChunks >= MaxQueuedAudioChunksAhead);

    if (!bBackpressured)
    {
        ExtractAndEnqueueSentences(bTimeout);
        if (bLLMFinished && LLMTextBuffer.Len() > 0)
        {
            ExtractAndEnqueueSentences(true);
        }
    }
    else
    {
        // Safety: keep buffer bounded while we wait for audio to catch up.
        if (MaxBufferedCharsWhileBackpressured > 0 && LLMTextBuffer.Len() > MaxBufferedCharsWhileBackpressured)
        {
            LLMTextBuffer = LLMTextBuffer.Right(MaxBufferedCharsWhileBackpressured);
        }
    }

    PumpAudioToProcedural();

    // Detect when audio playback has truly finished (procedural audio can gap between chunks).
    if (bLLMFinished && bAudioStarted && !bNotifiedAudioComplete)
    {
        const bool bAudioCurrentlyPlaying = AudioComp && AudioComp->IsPlaying();
        const bool bPipelineIdle =
            (PendingSentenceCount.GetValue() <= 0) &&
            (PendingAudioChunkCount.GetValue() <= 0) &&
            bAudioQueueDrained;

        const double LastProducedSec = Cycles64ToSecondsSafe(LastAudioChunkProducedCycles.GetValue());
        const double LastQueuedSec = Cycles64ToSecondsSafe(LastAudioChunkQueuedCycles.GetValue());
        const double LastActivitySec = FMath::Max(LastProducedSec, LastQueuedSec);
        const bool bRecentlyActive = (LastActivitySec > 0.0) && ((Now - LastActivitySec) < 0.35);

        if (!bPipelineIdle)
        {
            bAudioStoppedDetected = false;
        }
        else if (!bAudioCurrentlyPlaying && !bRecentlyActive)
        {
            if (!bAudioStoppedDetected)
            {
                bAudioStoppedDetected = true;
                AudioStoppedTime = Now;
            }
            else
            {
                const double GracePeriod = 0.45;
                if ((Now - AudioStoppedTime) >= GracePeriod)
                {
                    if (AudioComp && !AudioComp->IsPlaying() && bPipelineIdle && !bRecentlyActive)
                    {
                        bNotifiedAudioComplete = true;

                        if (UWorld* W = GetWorld())
                        {
                            if (ULocalTalkConversationSubsystem* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
                            {
                                Sub->NotifyAudioPlaybackFinished(this);
                            }
                        }
                    }
                }
            }
        }
        else
        {
            bAudioStoppedDetected = false;
        }
    }
}

void ULocalCharacterComponent::EnsureAudio()
{
    if (AudioComp && ProcWave) return;

    AActor* Owner = GetOwner();
    if (!Owner) return;

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
    }

    // If you couldn't hear anything, this is the most common reason: the audio was spatialized / attenuated.
    // Force 2D/UI audio by default so it's always audible while testing.
    if (bForce2DAudio)
    {
        AudioComp->bAllowSpatialization = false;
        AudioComp->bIsUISound = true;
    }

    ProcWave = NewObject<USoundWaveProcedural>(this, TEXT("LocalTalkerProcWave"));
    ProcWave->bLooping = false;

    // Default format; will be overridden once we load the first WAV chunk.
    ProcNumChannels = 1;
    ProcSampleRate = 22050;
    ProcWave->NumChannels = ProcNumChannels;
    ProcWave->SetSampleRate(ProcSampleRate);

    AudioComp->SetSound(ProcWave);
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

FString ULocalCharacterComponent::GetResolvedSpeakerName() const
{
    return GetSpeakerNameResolved();
}

bool ULocalCharacterComponent::IsSpeaking() const
{
    if (!bLLMFinished) return true;
    return IsAudioPlaying();
}

bool ULocalCharacterComponent::IsAudioPlaying() const
{
    if (!bAudioStarted) return false;
    if (!bLLMFinished) return true;
    if (PendingSentenceCount.GetValue() > 0) return true;
    if (PendingAudioChunkCount.GetValue() > 0) return true;
    if (!bAudioQueueDrained) return true;
    return AudioComp && AudioComp->IsPlaying();
}

void ULocalCharacterComponent::ReceiveBroadcastSpeech(const FString& InSpeakerName, const FString& Text, bool bFromUser)
{
    if (!bEnableProximityConversation) return;
    if (Text.IsEmpty()) return;

    const FString Role = bFromUser ? TEXT("User") : (InSpeakerName.IsEmpty() ? TEXT("Other") : InSpeakerName);
    History.Add({ Role, Text });
    TrimHistory();
}

void ULocalCharacterComponent::SendUserTextInterrupt(const FString& UserText, bool bInterrupt)
{
    if (UserText.IsEmpty()) return;
    if (!bEnableProximityConversation) return;

    if (UWorld* W = GetWorld())
    {
        if (ULocalTalkConversationSubsystem* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            const FVector Loc = GetOwner() ? GetOwner()->GetActorLocation() : FVector::ZeroVector;
            Sub->BroadcastUserUtterance(Loc, ConversationRadius, UserText, bInterrupt);
        }
    }
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
            if (!V.VoiceOnnxPath.IsEmpty() && FPaths::FileExists(V.VoiceOnnxPath))
            {
                Out.Add(V.Id.ToString());
            }
        }
    }

    // 2) Auto-discover any .onnx in the plugin voices folder
    if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LocalTalker")))
    {
        const FString VoicesDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/Voices"));
        TArray<FString> Found;
        IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
        PF.FindFiles(Found, *VoicesDir, TEXT(".onnx"));
        for (const FString& Path : Found)
        {
            if (!FPaths::FileExists(Path)) continue;
            const FString Base = FPathViews::GetCleanFilename(Path);
            FString Stem = Base;
            Stem.RemoveFromEnd(TEXT(".onnx"));
            Out.AddUnique(Stem);
        }
    }

    Out.Sort();
    return Out;
}

FString ULocalCharacterComponent::ResolveVoiceOnnxPath() const
{
    const ULocalTalkerSettings* S = GetDefault<ULocalTalkerSettings>();

    auto ResolveFromSettingsId = [&](FName Id) -> FString
    {
        if (!S || Id.IsNone()) return FString();
        for (const FLocalTalkVoiceOption& V : S->Voices)
        {
            if (V.Id == Id && !V.VoiceOnnxPath.IsEmpty() && FPaths::FileExists(V.VoiceOnnxPath))
            {
                return V.VoiceOnnxPath;
            }
        }
        return FString();
    };

    // 1) Component override
    if (!VoiceId.IsNone())
    {
        if (const FString P = ResolveFromSettingsId(VoiceId); !P.IsEmpty())
        {
            return P;
        }

        // fall back to auto-discovery (Id == filename stem)
        if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LocalTalker")))
        {
            const FString P = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/Voices"), VoiceId.ToString() + TEXT(".onnx"));
            if (FPaths::FileExists(P)) return P;
        }
    }

    // 2) First valid voice from settings
    if (S)
    {
        for (const FLocalTalkVoiceOption& V : S->Voices)
        {
            if (!V.Id.IsNone() && !V.VoiceOnnxPath.IsEmpty() && FPaths::FileExists(V.VoiceOnnxPath))
            {
                return V.VoiceOnnxPath;
            }
        }
    }

    // 3) Default fallback (bundled)
    if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LocalTalker")))
    {
        const FString P = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/Voices/en_US-lessac-small.onnx"));
        if (FPaths::FileExists(P)) return P;
    }

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

void ULocalCharacterComponent::EmitSubtitle(const FString& Text)
{
    const FString Speaker = GetSpeakerNameResolved();
    OnSubtitle.Broadcast(Speaker, Text);

    if (bShowOnScreenSubtitles)
    {
        DebugPrintLine(FString::Printf(TEXT("%s: %s"), *Speaker, *Text), OnScreenSubtitleSeconds, /*bNewLine*/ true);
    }
}

static FString TrimToLastNChars(const FString& In, int32 MaxChars)
{
    if (MaxChars <= 0) return FString();
    if (In.Len() <= MaxChars) return In;
    return In.Right(MaxChars);
}

FString ULocalCharacterComponent::BuildPromptWithHistory(const FLocalTalkerCharacterConfig& Config, const FString& UserText) const
{
    FString Directions = DirectionsPrompt;
    if (Directions.IsEmpty()) Directions = !Config.Directions.IsEmpty() ? Config.Directions : Config.SystemPrompt;

    FString Desc = CharacterDescriptionPrompt;
    if (Desc.IsEmpty()) Desc = !Config.CharacterDescription.IsEmpty() ? Config.CharacterDescription : Config.Persona;

    const int32 UseMaxContextChars = (MaxContextChars != 1600) ? MaxContextChars : Config.MaxContextChars;

    FString P;
    if (!Directions.IsEmpty())
    {
        P += TEXT("Directions:\n") + Directions + TEXT("\n\n");
    }
    if (!Desc.IsEmpty())
    {
        P += TEXT("Character Description:\n") + Desc + TEXT("\n\n");
    }

    FString Transcript;
    if (bUseConversationHistory)
    {
        for (const FChatMsg& M : History)
        {
            Transcript += M.Role + TEXT(": ") + M.Content + TEXT("\n");
        }
    }
    else
    {
        Transcript += TEXT("User: ") + UserText + TEXT("\n");
    }

    Transcript = TrimToLastNChars(Transcript, UseMaxContextChars);
    P += Transcript;

    if (!P.EndsWith(TEXT("\n"))) P += TEXT("\n");
    P += TEXT("Assistant:");
    return P;
}

void ULocalCharacterComponent::TrimHistory()
{
    const int32 UseMaxHistoryMessages = (MaxHistoryMessages != 16) ? MaxHistoryMessages : ResolveConfig().MaxHistoryMessages;
    if (UseMaxHistoryMessages <= 0)
    {
        History.Reset();
        return;
    }
    while (History.Num() > UseMaxHistoryMessages)
    {
        History.RemoveAt(0);
    }
}

void ULocalCharacterComponent::EnsureProcWaveFormat(int32 SampleRate, int32 NumChannels)
{
    // If unknown/invalid, keep the existing format.
    if (SampleRate <= 0 || NumChannels <= 0) return;
    if (!ProcWave) return;

    if (ProcSampleRate == SampleRate && ProcNumChannels == NumChannels) return;

    // Changing format mid-stream is risky; reset audio and restart playback.
    if (AudioComp) AudioComp->Stop();
    ProcWave->ResetAudio();
    bAudioStarted = false;

    ProcSampleRate = SampleRate;
    ProcNumChannels = NumChannels;
    ProcWave->NumChannels = ProcNumChannels;
    ProcWave->SetSampleRate(ProcSampleRate);
}

FLocalTalkerRuntimePaths ULocalCharacterComponent::ResolvePaths() const
{
    if (!bUseProjectSettingsPaths) return PathsOverride;

    const ULocalTalkerSettings* S = GetDefault<ULocalTalkerSettings>();
    FLocalTalkerRuntimePaths Out = S ? S->DefaultPaths : FLocalTalkerRuntimePaths();

    if (!PathsOverride.LlamaModelPath.IsEmpty()) Out.LlamaModelPath = PathsOverride.LlamaModelPath;
    if (!PathsOverride.LlamaLibPath.IsEmpty()) Out.LlamaLibPath = PathsOverride.LlamaLibPath;
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

        if (Out.LlamaModelPath.IsEmpty())
        {
            // Default shipped model path (we'll bundle at least one small, permissive GGUF)
            Out.LlamaModelPath = FPaths::Combine(Base, TEXT("Resources/Models/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"));
        }

        if (Out.PiperExePath.IsEmpty())
        {
            Out.PiperExePath = FPaths::Combine(Base, TEXT("ThirdParty/piper/Win64/Release/piper.exe"));
        }

        if (Out.PiperVoiceModelPath.IsEmpty())
        {
            // Default shipped voice model (we'll bundle at least one fast, lightweight voice)
            Out.PiperVoiceModelPath = FPaths::Combine(Base, TEXT("Resources/Voices/en_US-lessac-small.onnx"));
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

    Out.MaxContextChars = CharacterConfigOverride.MaxContextChars != 1600 ? CharacterConfigOverride.MaxContextChars : Out.MaxContextChars;
    Out.MaxHistoryMessages = CharacterConfigOverride.MaxHistoryMessages != 16 ? CharacterConfigOverride.MaxHistoryMessages : Out.MaxHistoryMessages;

    Out.MaxTokens = CharacterConfigOverride.MaxTokens != 192 ? CharacterConfigOverride.MaxTokens : Out.MaxTokens;
    Out.Temperature = CharacterConfigOverride.Temperature != 0.7f ? CharacterConfigOverride.Temperature : Out.Temperature;
    Out.Seed = CharacterConfigOverride.Seed != 0 ? CharacterConfigOverride.Seed : Out.Seed;

    if (!CharacterConfigOverride.Stop.IsEmpty()) Out.Stop = CharacterConfigOverride.Stop;

    Out.bSpeak = CharacterConfigOverride.bSpeak;
    Out.bStreamTokens = CharacterConfigOverride.bStreamTokens;

    return Out;
}

void ULocalCharacterComponent::Interrupt()
{
    bInterrupted = true;
    bLLMFinished = true;
    bAudioQueueDrained = true;
    bNotifiedAudioComplete = true; // don't fire audio completion on interrupt
    bAudioStoppedDetected = false;
    PendingSentenceCount.Set(0);
    PendingAudioChunkCount.Set(0);
    LastAudioChunkQueuedCycles.Set(0);
    LastAudioChunkProducedCycles.Set(0);

    if (ActiveLLM)
    {
        ActiveLLM->Cancel();
    }

    LLMTextBuffer.Reset();
    LLMFullText.Reset();

    FString Tmp;
    while (SentenceQueue.Dequeue(Tmp)) {}

    FAudioChunk Buf;
    while (AudioQueue.Dequeue(Buf)) {}

    if (AudioComp) AudioComp->Stop();
    if (ProcWave) ProcWave->ResetAudio();

    bAudioStarted = false;
    ActiveLLM = nullptr;

    if (UWorld* W = GetWorld())
    {
        if (ULocalTalkConversationSubsystem* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            Sub->NotifyInterrupted(this);
        }
    }

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
    History.Reset();
}

void ULocalCharacterComponent::SpeakTextLocal(const FString& Text)
{
    bInterrupted = false;
    bLLMFinished = true;
    bAudioQueueDrained = false;
    bNotifiedAudioComplete = false;
    bAudioStoppedDetected = false;
    PendingSentenceCount.Set(0);
    PendingAudioChunkCount.Set(0);
    LastAudioChunkQueuedCycles.Set(0);
    LastAudioChunkProducedCycles.Set(0);

    EnsureAudio();

    if (UWorld* W = GetWorld())
    {
        if (ULocalTalkConversationSubsystem* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            Sub->NotifyStartedSpeaking(this);
        }
    }

    EnqueueSentence(Text);
    bAudioQueueDrained = true; // no more sentences will be queued for SpeakTextLocal

    if (UWorld* W = GetWorld())
    {
        if (ULocalTalkConversationSubsystem* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            Sub->NotifyFinishedSpeaking(this);
        }
    }

    const FLocalTalkerRuntimePaths Paths = ResolvePaths();
    StartTTSWorker(Paths);
}

void ULocalCharacterComponent::SendPromptAndSpeakStreamingInProc(const FString& Prompt)
{
    bInterrupted = false;
    bLLMFinished = false;
    bAudioQueueDrained = false;
    bNotifiedAudioComplete = false;
    bAudioStoppedDetected = false;
    PendingSentenceCount.Set(0);
    PendingAudioChunkCount.Set(0);
    LastAudioChunkQueuedCycles.Set(0);
    LastAudioChunkProducedCycles.Set(0);

    LLMTextBuffer.Reset();
    LLMFullText.Reset();
    LastTextAppendSeconds = FPlatformTime::Seconds();

    EnsureAudio();

    const FLocalTalkerRuntimePaths Paths = ResolvePaths();
    const FLocalTalkerCharacterConfig Config = ResolveConfig();

    StartTTSWorker(Paths);

    if (UWorld* W = GetWorld())
    {
        if (ULocalTalkConversationSubsystem* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            Sub->NotifyStartedSpeaking(this);
        }
    }

    if (!Prompt.IsEmpty())
    {
        History.Add({ TEXT("User"), Prompt });
        TrimHistory();
    }

    const FString PromptText = BuildPromptWithHistory(Config, Prompt);
    ActiveLLM = ULocalTalkerInProcGenerateAsync::GenerateStreamingInProcWithPromptText(this, Paths, Config, PromptText);
    ActiveLLM->OnToken.AddDynamic(this, &ULocalCharacterComponent::HandleLLMToken);
    ActiveLLM->OnDelta.AddDynamic(this, &ULocalCharacterComponent::HandleLLMDelta);
    ActiveLLM->OnCompleted.AddDynamic(this, &ULocalCharacterComponent::HandleLLMCompleted);
    ActiveLLM->OnError.AddDynamic(this, &ULocalCharacterComponent::HandleLLMError);
    ActiveLLM->Activate();
}

void ULocalCharacterComponent::HandleLLMError(const FString& Error)
{
    UE_LOG(LogLocalTalker, Error, TEXT("[%s] LLM error: %s"), *GetSpeakerNameResolved(), *Error);
    if (bDebugPrintGeneratedText)
    {
        DebugPrintLine(FString::Printf(TEXT("[%s] LLM ERROR: %s"), *GetSpeakerNameResolved(), *Error), 6.0f);
    }
    OnError.Broadcast(Error);
    bLLMFinished = true;
    bAudioQueueDrained = true;

    if (UWorld* W = GetWorld())
    {
        if (ULocalTalkConversationSubsystem* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            Sub->NotifyInterrupted(this);
        }
    }
}

void ULocalCharacterComponent::HandleLLMToken(const FString& Token)
{
    if (bDebugLogTokens)
    {
        UE_LOG(LogLocalTalker, Verbose, TEXT("[%s] token: %s"), *GetSpeakerNameResolved(), *Token);
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
        DebugPrintLine(FString::Printf(TEXT("[%s] %s"), *GetSpeakerNameResolved(), *Tail), 1.0f);
    }
}

void ULocalCharacterComponent::HandleLLMCompleted(const FString& Text)
{
    // Text is the full completion (per async node contract).
    bLLMFinished = true;
    bAudioQueueDrained = true; // no more sentences will be enqueued
    OnSpokenText.Broadcast(Text);

    if (!Text.IsEmpty())
    {
        History.Add({ TEXT("Assistant"), Text });
        TrimHistory();
    }

    if (UWorld* W = GetWorld())
    {
        if (ULocalTalkConversationSubsystem* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            Sub->NotifyFinishedSpeaking(this);
        }
    }
}

void ULocalCharacterComponent::EnqueueSentence(const FString& Sentence)
{
    const FString S = TrimSentence(Sentence);
    if (S.Len() <= 0) return;
    PendingSentenceCount.Increment();
    SentenceQueue.Enqueue(S);
    EmitSubtitle(S);
    UE_LOG(LogLocalTalker, Log, TEXT("[%s] Enqueued sentence (%d chars)"), *GetSpeakerNameResolved(), S.Len());

    if (bEnableProximityConversation)
    {
        if (UWorld* W = GetWorld())
        {
            if (ULocalTalkConversationSubsystem* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
            {
                Sub->NotifySentenceSpoken(this, S, /*bFromUser*/ false);
            }
        }
    }
}

void ULocalCharacterComponent::ExtractAndEnqueueSentences(bool bForceFlush)
{
    if (!bSpeakStreaming) return;
    if (LLMTextBuffer.Len() < MinCharsBeforeSpeak && !bForceFlush) return;

    FString Work = LLMTextBuffer;
    Work.ReplaceInline(TEXT("\r"), TEXT(""));

    int32 CutIdx = INDEX_NONE;
    for (int32 i = 0; i < Work.Len(); i++)
    {
        if (IsSentenceTerminator(Work[i]))
        {
            CutIdx = i;
            if (i + 1 >= MinCharsBeforeSpeak) break;
        }

        if (i >= MaxSentenceChars)
        {
            CutIdx = i;
            break;
        }
    }

    if (CutIdx == INDEX_NONE)
    {
        if (bForceFlush && Work.Len() > 0)
        {
            EnqueueSentence(Work);
            LLMTextBuffer.Reset();
        }
        return;
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
        TQueue<TArray<uint8>, EQueueMode::Mpsc>& InAudioQueue,
        FThreadSafeBool& InStop,
        ULocalCharacterComponent* InOwner,
        const FLocalTalkerRuntimePaths& InPaths
    )
        : SentenceQueue(InSentenceQueue)
        , AudioQueue(InAudioQueue)
        , bStop(InStop)
        , Owner(InOwner)
        , Paths(InPaths)
    {}

    virtual uint32 Run() override
    {
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

            // Sentence consumed; allow completion tracking.
            Owner->PendingSentenceCount.Decrement();

            FString Err;
            Owner->RunPiperSentenceToAudioQueue(Sentence, Paths, Err);
            if (!Err.IsEmpty())
            {
                AsyncTask(ENamedThreads::GameThread, [Owner = Owner, Err]()
                {
                    if (Owner) Owner->OnError.Broadcast(Err);
                });
            }
        }
        return 0;
    }

private:
    TQueue<FString, EQueueMode::Mpsc>& SentenceQueue;
    TQueue<TArray<uint8>, EQueueMode::Mpsc>& AudioQueue;
    FThreadSafeBool& bStop;
    ULocalCharacterComponent* Owner = nullptr;
    FLocalTalkerRuntimePaths Paths;
};

void ULocalCharacterComponent::StartTTSWorker(const FLocalTalkerRuntimePaths& Paths)
{
    if (bTTSWorkerRunning) return;

    bTTSWorkerRunning = true;
    bTTSStop = false;

    TTSRunnable = new FLocalTalkerTTSWorker(SentenceQueue, AudioQueue, bTTSStop, this, Paths);
    TTSThread = FRunnableThread::Create(TTSRunnable, TEXT("LocalTalkerTTSWorker"), 0, TPri_BelowNormal);
}

void ULocalCharacterComponent::StopTTSWorker()
{
    if (!bTTSWorkerRunning) return;

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
}

void ULocalCharacterComponent::RunPiperSentenceToAudioQueue(const FString& Sentence, const FLocalTalkerRuntimePaths& Paths, FString& OutErr)
{
    const FString VoicePath = ResolveVoiceOnnxPath();
    if (Paths.PiperExePath.IsEmpty() || VoicePath.IsEmpty())
    {
        OutErr = TEXT("Piper paths not set. Configure Project Settings -> LocalTalker (PiperExePath + Voices).");
        return;
    }

    UE_LOG(LogLocalTalker, Log, TEXT("[%s] Piper start: %s"), *GetSpeakerNameResolved(), *Sentence);

    const FString TempDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("LocalTalker"));
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*TempDir);

    const FString OutWav = FPaths::Combine(TempDir, FString::Printf(TEXT("tts_%llu.wav"), (uint64)FPlatformTime::Cycles64()));

    FString Args;
    Args += TEXT("-m ") + QuoteArg3(VoicePath) + TEXT(" ");
    Args += TEXT("-f ") + QuoteArg3(OutWav) + TEXT(" ");

    FProcHandle Handle;
    FLocalProcPipes Pipes;
    FString SpawnError;

    if (!FLocalTalkerProcess::SpawnWithPipes(Paths.PiperExePath, Args, Paths.WorkingDir, Handle, Pipes, SpawnError))
    {
        OutErr = SpawnError;
        UE_LOG(LogLocalTalker, Error, TEXT("[%s] Piper spawn failed: %s"), *GetSpeakerNameResolved(), *OutErr);
        return;
    }

    FLocalTalkerProcess::WriteStdin(Pipes, Sentence + TEXT("\n"));
    if (Pipes.WriteInPipe)
    {
        FPlatformProcess::ClosePipe(nullptr, Pipes.WriteInPipe);
        Pipes.WriteInPipe = nullptr;
    }

    FString StdErrAll;
    auto OnErr = [&](const FString& Chunk) { StdErrAll += Chunk; };
    auto OnOut = [&](const FString&) {};

    FLocalTalkerProcess::PumpOutputUntilExit(Handle, Pipes, OnOut, OnErr, 0.005);

    int32 ReturnCode = 0;
    FPlatformProcess::GetProcReturnCode(Handle, &ReturnCode);
    FPlatformProcess::CloseProc(Handle);
    FLocalTalkerProcess::ClosePipes(Pipes);

    if (ReturnCode != 0)
    {
        OutErr = FString::Printf(TEXT("piper failed (code %d). stderr:\n%s"), ReturnCode, *StdErrAll);
        UE_LOG(LogLocalTalker, Error, TEXT("[%s] %s"), *GetSpeakerNameResolved(), *OutErr);
        return;
    }

    FLocalWavPcm16 W;
    FString WavErr;
    if (!FLocalTalkerWav::LoadWavPcm16(OutWav, W, WavErr))
    {
        OutErr = WavErr;
        UE_LOG(LogLocalTalker, Error, TEXT("[%s] WAV load failed: %s"), *GetSpeakerNameResolved(), *OutErr);
        return;
    }

    if (W.Samples.Num() == 0) return;

    // Clean up the temp file ASAP; we have the audio in memory now.
    PF.DeleteFile(*OutWav);

    // Piper voices are typically mono, but handle basic stereo->mono downmix if needed.
    int32 NumChannels = FMath::Max(1, W.NumChannels);
    int32 SampleRate = FMath::Max(1, W.SampleRate);

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
        return;
    }

    // NOTE: We now apply format per-chunk in PumpAudioToProcedural() before QueueAudio(),
    // which prevents sample-rate mismatch “fast speech” at startup.

    const TArray<int16>& Use = (Mono.Num() > 0) ? Mono : W.Samples;
    const float DurationSec = (SampleRate > 0 && NumChannels > 0)
        ? ((float)Use.Num() / (float)(SampleRate * NumChannels))
        : 0.0f;

    if (bUseUESubtitles && DurationSec > 0.0f)
    {
        const PTRINT SubtitleId = (PTRINT)this;
        const float Priority = UESubtitlePriority;
        const FString Line = FString::Printf(TEXT("%s: %s"), *GetSpeakerNameResolved(), *Sentence);
        TWeakObjectPtr<UWorld> WorldPtr = GetWorld();

        AsyncTask(ENamedThreads::GameThread, [SubtitleId, Priority, DurationSec, Line, WorldPtr]()
        {
            UWorld* World = WorldPtr.Get();
            if (!World) return;

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
                DurationSec,
                Cues,
                /*InStartTime*/ 0.0f,
                World->GetAudioTimeSeconds()
            );
        });
    }

    FAudioChunk Chunk;
    Chunk.SampleRate = SampleRate;
    Chunk.NumChannels = NumChannels;
    Chunk.Bytes.SetNumUninitialized(Use.Num() * sizeof(int16));
    FMemory::Memcpy(Chunk.Bytes.GetData(), Use.GetData(), Chunk.Bytes.Num());

    PendingAudioChunkCount.Increment();
    LastAudioChunkProducedCycles.Set((int64)FPlatformTime::Cycles64());
    AudioQueue.Enqueue(MoveTemp(Chunk));

    UE_LOG(LogLocalTalker, Log, TEXT("[%s] Piper ok: %d samples, %d ch, %d Hz (queued)"),
        *GetSpeakerNameResolved(),
        W.Samples.Num(),
        NumChannels,
        SampleRate
    );
}

void ULocalCharacterComponent::PumpAudioToProcedural()
{
    EnsureAudio();

    if (!AudioComp || !ProcWave) return;
    if (bInterrupted) return;

    FAudioChunk Chunk;
    bool bQueued = false;

    for (int32 i = 0; i < 8; i++)
    {
        if (!AudioQueue.Dequeue(Chunk)) break;
        if (Chunk.Bytes.Num() == 0) continue;

        // Ensure format is correct BEFORE queueing audio (prevents sample-rate mismatch “fast speech”).
        EnsureProcWaveFormat(Chunk.SampleRate, Chunk.NumChannels);

        ProcWave->QueueAudio(Chunk.Bytes.GetData(), Chunk.Bytes.Num());
        PendingAudioChunkCount.Decrement();
        LastAudioChunkQueuedCycles.Set((int64)FPlatformTime::Cycles64());
        bQueued = true;
    }

    if (bQueued && !bAudioStarted)
    {
        AudioComp->SetSound(ProcWave);
        AudioComp->Play();
        bAudioStarted = true;
        AudioPlaybackStartTime = FPlatformTime::Seconds();
        UE_LOG(LogLocalTalker, Log, TEXT("[%s] Audio started (procedural)."), *GetSpeakerNameResolved());
    }

    // If we underflowed and the component stopped, restart when new audio arrives.
    if (bQueued && bAudioStarted && AudioComp && !AudioComp->IsPlaying())
    {
        AudioComp->Play();
    }
}

#if WITH_DEV_AUTOMATION_TESTS
void ULocalCharacterComponent::Test_SetLLMTextBuffer(const FString& InText)
{
    LLMTextBuffer = InText;
    bInterrupted = false;
}

int32 ULocalCharacterComponent::Test_GetLLMTextBufferLen() const
{
    return LLMTextBuffer.Len();
}

void ULocalCharacterComponent::Test_SetPendingCounts(int32 InPendingSentences, int32 InPendingAudioChunks)
{
    PendingSentenceCount.Set(InPendingSentences);
    PendingAudioChunkCount.Set(InPendingAudioChunks);
}

int32 ULocalCharacterComponent::Test_GetPendingSentenceCount() const
{
    return PendingSentenceCount.GetValue();
}

int32 ULocalCharacterComponent::Test_GetPendingAudioChunkCount() const
{
    return PendingAudioChunkCount.GetValue();
}

void ULocalCharacterComponent::Test_InitAudio()
{
    EnsureAudio();
}

void ULocalCharacterComponent::Test_EnqueueAudioChunk(int32 InSampleRate, int32 InNumChannels, int32 InNumSamples)
{
    FAudioChunk Chunk;
    Chunk.SampleRate = InSampleRate;
    Chunk.NumChannels = InNumChannels;

    const int32 NumSamples = FMath::Max(0, InNumSamples);
    Chunk.Bytes.SetNumZeroed(NumSamples * sizeof(int16));

    PendingAudioChunkCount.Increment();
    AudioQueue.Enqueue(MoveTemp(Chunk));
}

void ULocalCharacterComponent::Test_PumpAudio()
{
    PumpAudioToProcedural();
}

void ULocalCharacterComponent::Test_GetProcFormat(int32& OutSampleRate, int32& OutNumChannels) const
{
    OutSampleRate = ProcSampleRate;
    OutNumChannels = ProcNumChannels;
}
#endif
