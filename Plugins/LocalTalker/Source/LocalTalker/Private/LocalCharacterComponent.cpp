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
#include "Misc/Guid.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"

static TAutoConsoleVariable<int32> CVarLocalTalkerTraceConversation(
    TEXT("LocalTalker.TraceConversation"),
    0,
    TEXT("Enable high-signal conversation tracing logs.\n0=off, 1=on"),
    ECVF_Default
);

static bool TraceEnabled(const ULocalCharacterComponent* C)
{
    return (CVarLocalTalkerTraceConversation.GetValueOnAnyThread() != 0) || (C && C->bTraceConversation);
}

static FString LocalTalkerOneLineTrunc(const FString& In, int32 MaxChars)
{
    FString S = In.Replace(TEXT("\r"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" ")).TrimStartAndEnd();
    return (MaxChars > 0 && S.Len() > MaxChars) ? S.Left(MaxChars) + TEXT("...") : S;
}

// Best-effort cleanup so model output becomes "dialogue", not chat transcripts.
// - Collapses whitespace/newlines
// - Removes leading role/speaker prefixes ("Assistant:", "User:", "Milo:", etc.)
static FString LocalTalkerSanitizeDialogueLine(const FString& In)
{
    // Normalize line breaks -> spaces
    FString S = In;
    S.ReplaceInline(TEXT("\r"), TEXT(" "));
    S.ReplaceInline(TEXT("\n"), TEXT(" "));

    // Collapse whitespace runs
    FString Out;
    Out.Reserve(S.Len());
    bool bPrevWs = false;
    for (int32 i = 0; i < S.Len(); i++)
    {
        const TCHAR C = S[i];
        const bool bWs = FChar::IsWhitespace(C) != 0;
        if (bWs)
        {
            if (!bPrevWs)
            {
                Out.AppendChar(TEXT(' '));
                bPrevWs = true;
            }
            continue;
        }
        bPrevWs = false;
        Out.AppendChar(C);
    }
    Out.TrimStartAndEndInline();

    // Strip known prefixes repeatedly (models sometimes emit "Assistant: Assistant: ...").
    // Keep this small + deterministic; if the prompt/template is correct, this should rarely trigger.
    auto StripPrefix = [&Out](const TCHAR* Prefix) -> bool
    {
        const int32 PrefixLen = FCString::Strlen(Prefix);
        if (Out.Len() >= PrefixLen && Out.Left(PrefixLen).Equals(Prefix, ESearchCase::IgnoreCase))
        {
            Out = Out.Mid(PrefixLen);
            Out.TrimStartAndEndInline();
            return true;
        }
        return false;
    };

    bool bStrippedAny = false;
    for (;;)
    {
        bool bStrippedThisLoop = false;

        bStrippedThisLoop |= StripPrefix(TEXT("Assistant:"));
        bStrippedThisLoop |= StripPrefix(TEXT("User:"));
        bStrippedThisLoop |= StripPrefix(TEXT("System:"));
        bStrippedThisLoop |= StripPrefix(TEXT("NPC:"));
        bStrippedThisLoop |= StripPrefix(TEXT("<|assistant|>"));
        bStrippedThisLoop |= StripPrefix(TEXT("<|user|>"));
        bStrippedThisLoop |= StripPrefix(TEXT("<|system|>"));
        bStrippedThisLoop |= StripPrefix(TEXT("</s>"));
        bStrippedThisLoop |= StripPrefix(TEXT("[INST]"));
        bStrippedThisLoop |= StripPrefix(TEXT("[/INST]"));

        // Generic "Name:" prefix (single token up to 24 chars, no spaces).
        int32 ColonIdx = Out.Find(TEXT(":"), ESearchCase::IgnoreCase, ESearchDir::FromStart);
        if (ColonIdx > 0 && ColonIdx <= 24)
        {
            const FString Left = Out.Left(ColonIdx);
            if (!Left.Contains(TEXT(" ")) && !Left.Contains(TEXT("\t")))
            {
                Out = Out.Mid(ColonIdx + 1);
                Out.TrimStartAndEndInline();
                bStrippedThisLoop = true;
            }
        }

        bStrippedAny |= bStrippedThisLoop;
        if (!bStrippedThisLoop) break;
    }

    // If we stripped a prefix, do one more whitespace collapse (prefix removal can expose double spaces)
    if (bStrippedAny)
    {
        Out.ReplaceInline(TEXT("  "), TEXT(" "));
        Out.TrimStartAndEndInline();
    }

    return Out;
}

ULocalCharacterComponent::ULocalCharacterComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
}

void ULocalCharacterComponent::BeginPlay()
{
    Super::BeginPlay();
    if (UWorld* W = GetWorld())
    {
        if (auto* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            Sub->RegisterTalker(this);
        }
    }
}

void ULocalCharacterComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
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

    ExtractAndEnqueueSentences(false);
    PumpAudioToProcedural();

    // If the procedural audio is "stuck playing" after the last chunk, force-stop it after a short tail.
    // This prevents long turn handoff delays between speakers.
    if (bIsSpeakingInternal && bLLMFinished && bAudioQueueDrained && PendingSentenceCount.GetValue() == 0 && PendingAudioChunkCount.GetValue() == 0)
    {
        if (AudioComp && AudioComp->IsPlaying())
        {
            const uint64 Enq = LastAudioEnqueueCycles.Load();
            if (Enq != 0)
            {
                const double Since = FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - Enq);
                if (Since >= (double)TurnReleaseAudioTailSeconds)
                {
                    AudioComp->Stop();
                }
            }
        }
    }

    // Check if we finished our turn
    if (bIsSpeakingInternal && bLLMFinished && PendingSentenceCount.GetValue() == 0 && PendingAudioChunkCount.GetValue() == 0 && bAudioQueueDrained)
    {
        // One final check: is the audio component actually finished?
        if (!AudioComp || !AudioComp->IsPlaying())
        {
            if (!bNotifiedSubsystemFinished)
            {
                bNotifiedSubsystemFinished = true;
                bIsSpeakingInternal = false;
                
                if (UWorld* W = GetWorld())
                {
                    if (auto* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
                    {
                        Sub->ReleaseTurn(this);
                    }
                }

                // Reset for next turn after informing the Director.
                bSpokeThisTurn = false;
            }
        }
    }
}

void ULocalCharacterComponent::SendPromptAndSpeakStreamingInProc(const FString& Prompt)
{
    if (UWorld* W = GetWorld())
    {
        if (auto* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            Sub->RequestTurn(this, Prompt);
        }
    }
}

void ULocalCharacterComponent::SpeakTextLocal(const FString& Text)
{
    if (UWorld* W = GetWorld())
    {
        if (auto* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            // We use a prefix to identify raw text vs prompt
            Sub->RequestTurn(this, TEXT("RAW:") + Text);
        }
    }
}

void ULocalCharacterComponent::InternalGrantTurn(const FString& PromptOrText)
{
    bIsSpeakingInternal = true;
    bLLMFinished = false;
    bAudioQueueDrained = false;
    bNotifiedSubsystemFinished = false;
    bInterrupted = false;
    bSpokeThisTurn = false;
    LLMTextBuffer.Reset();
    LLMFullText.Reset();

    EnsureAudio();
    StartTTSWorker(ResolvePaths());

    if (PromptOrText.StartsWith(TEXT("RAW:")))
    {
        FString CleanText = PromptOrText.Mid(4);
        bLLMFinished = true;
        EnqueueSentence(CleanText);
        bAudioQueueDrained = true;
    }
    else
    {
        // IMPORTANT:
        // Use the in-proc backend's own prompt wrapper (see LocalTalkerInProcAsync::BuildPrompt),
        // which includes an output contract ("ONE line of spoken dialogue") and avoids chatty
        // "User:" / "Assistant:" transcript leakage.
        //
        // We still include short rolling history in the *user prompt* so the model has context.
        FLocalTalkerCharacterConfig Config = ResolveConfig();

        // Map per-character fields into the config used by the prompt wrapper.
        if (!Directions.IsEmpty()) Config.Directions = Directions;
        if (!Desc.IsEmpty()) Config.CharacterDescription = Desc;

        FString UserPrompt = PromptOrText;
        if (UWorld* W = GetWorld())
        {
            if (auto* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
            {
                const TArray<FLocalTalkMessage> FullHistory = Sub->GetContextHistory(this);
                if (FullHistory.Num() > 0)
                {
                    FString H;
                    H += TEXT("Conversation so far:\n");
                    for (const auto& M : FullHistory)
                    {
                        // History may contain old transcript-y content; sanitize lightly for better conditioning.
                        const FString Clean = LocalTalkerSanitizeDialogueLine(M.Content);
                        if (!Clean.IsEmpty())
                        {
                            // Avoid "Name: ..." formatting in the conditioning text; it encourages the model
                            // to emit speaker-label transcripts. Prefer a narrative framing.
                            H += FString::Printf(TEXT("%s said \"%s\".\n"), *M.SpeakerName, *Clean);
                        }
                    }
                    H += TEXT("\n");
                    H += UserPrompt;
                    UserPrompt = MoveTemp(H);
                }
            }
        }

        ActiveLLM = ULocalTalkerInProcGenerateAsync::GenerateStreamingInProc(this, ResolvePaths(), Config, UserPrompt);
        ActiveLLM->OnToken.AddDynamic(this, &ULocalCharacterComponent::HandleLLMToken);
        ActiveLLM->OnDelta.AddDynamic(this, &ULocalCharacterComponent::HandleLLMDelta);
        ActiveLLM->OnCompleted.AddDynamic(this, &ULocalCharacterComponent::HandleLLMCompleted);
        ActiveLLM->OnError.AddDynamic(this, &ULocalCharacterComponent::HandleLLMError);
        ActiveLLM->Activate();
    }
}

void ULocalCharacterComponent::OnHeardSpeech(const FString& InSpeakerName, const FString& Text, bool bFromUser)
{
    // Personal history has been removed in favor of the centralized Subsystem history.
    // We only log here for debug purposes.

    if (TraceEnabled(this))
    {
        UE_LOG(LogLocalTalker, Log, TEXT("[%s] Heard from %s: %s"), 
            *GetSpeakerNameResolved(), *InSpeakerName, *LocalTalkerOneLineTrunc(Text, 40));
    }
}

void ULocalCharacterComponent::Interrupt()
{
    bInterrupted = true;
    bLLMFinished = true;
    bAudioQueueDrained = true;
    bSpokeThisTurn = false;
    if (AudioComp) AudioComp->Stop();
    if (ActiveLLM) ActiveLLM->Cancel();
    
    if (bIsSpeakingInternal && !bNotifiedSubsystemFinished)
    {
        bNotifiedSubsystemFinished = true;
        bIsSpeakingInternal = false;
        if (UWorld* W = GetWorld())
        {
            if (auto* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
            {
                Sub->ReleaseTurn(this);
            }
        }
    }
}

void ULocalCharacterComponent::ClearConversation()
{
    // Centralized history should be cleared via the Subsystem if needed.
}

FString ULocalCharacterComponent::GetSpeakerNameResolved() const
{
    if (!SpeakerName.IsEmpty()) return SpeakerName;
    return GetOwner() ? GetOwner()->GetActorLabel() : TEXT("None");
}

// --- Internal Machinery ---

void ULocalCharacterComponent::EnqueueSentence(const FString& Sentence)
{
    FString S = LocalTalkerSanitizeDialogueLine(Sentence);
    if (S.IsEmpty()) return;

    bSpokeThisTurn = true;
    PendingSentenceCount.Increment();
    SentenceQueue.Enqueue(S);
    EmitSubtitle(S);

    if (UWorld* W = GetWorld())
    {
        if (auto* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            Sub->BroadcastSentence(this, S, false);
        }
    }
}

void ULocalCharacterComponent::HandleLLMCompleted(const FString& Text)
{
    ExtractAndEnqueueSentences(true);
    bLLMFinished = true;
    bAudioQueueDrained = true;
}

void ULocalCharacterComponent::HandleLLMToken(const FString& Token)
{
    LLMTextBuffer += Token;
    LLMFullText += Token;
}

void ULocalCharacterComponent::HandleLLMDelta(const FString& Text) {}
void ULocalCharacterComponent::HandleLLMError(const FString& Error)
{
    // IMPORTANT: Previously this silently swallowed errors from the LLM async node,
    // making it look like characters "finish talking" instantly with no output.
    // Common cause: missing Project Settings -> LocalTalker -> DefaultPaths (LlamaLibPath / LlamaModelPath).

    UE_LOG(LogLocalTalker, Warning, TEXT("[%s] LLM error: %s"), *GetSpeakerNameResolved(), *Error);
    OnError.Broadcast(Error);

    if (bShowOnScreenSubtitles && GEngine)
    {
        GEngine->AddOnScreenDebugMessage(
            -1,
            6.0f,
            FColor::Red,
            FString::Printf(TEXT("%s LLM error: %s"), *GetSpeakerNameResolved(), *Error)
        );
    }

    // Mark as finished so the Director can release the turn; we intentionally DO NOT set bSpokeThisTurn.
    bLLMFinished = true;
    bAudioQueueDrained = true;
}

void ULocalCharacterComponent::ExtractAndEnqueueSentences(bool bForceFlush)
{
    if (LLMTextBuffer.Len() < MinCharsBeforeSpeak && !bForceFlush) return;

    int32 CutIdx = INDEX_NONE;
    for (int32 i = 0; i < LLMTextBuffer.Len(); i++)
    {
        TCHAR C = LLMTextBuffer[i];
        if (C == '.' || C == '!' || C == '?' || C == '\n')
        {
            CutIdx = i;
            if (i + 1 >= MinCharsBeforeSpeak) break;
        }
    }

    if (CutIdx != INDEX_NONE || (bForceFlush && LLMTextBuffer.Len() > 0))
    {
        int32 Len = (CutIdx != INDEX_NONE) ? CutIdx + 1 : LLMTextBuffer.Len();
        FString S = LLMTextBuffer.Left(Len);
        LLMTextBuffer = LLMTextBuffer.Mid(Len);
        EnqueueSentence(S);
    }
}

void ULocalCharacterComponent::PumpAudioToProcedural()
{
    if (!ProcWave) return;
    
    FAudioChunk Chunk;
    while (AudioQueue.Dequeue(Chunk))
    {
        PendingAudioChunkCount.Decrement();

        // Ensure the procedural wave matches the chunk format.
        if (Chunk.SampleRate > 0 && ProcWave->GetSampleRateForCurrentPlatform() != Chunk.SampleRate)
        {
            ProcWave->SetSampleRate(Chunk.SampleRate);
        }
        if (Chunk.NumChannels > 0 && ProcWave->NumChannels != Chunk.NumChannels)
        {
            ProcWave->NumChannels = Chunk.NumChannels;
        }

        ProcWave->QueueAudio(Chunk.Bytes.GetData(), Chunk.Bytes.Num());
        if (AudioComp && !AudioComp->IsPlaying()) AudioComp->Play();
    }
}

void ULocalCharacterComponent::EnsureAudio()
{
    if (!AudioComp)
    {
        AudioComp = NewObject<UAudioComponent>(GetOwner());
        AudioComp->RegisterComponent();
    }
    if (!ProcWave)
    {
        ProcWave = NewObject<USoundWaveProcedural>();
        ProcWave->SetSampleRate(16000);
        ProcWave->NumChannels = 1;
        ProcWave->Duration = INDEFINITELY_LOOPING_DURATION;
        ProcWave->bLooping = false;
    }
    if (AudioComp->GetSound() != ProcWave)
    {
        AudioComp->SetSound(ProcWave);
    }
}

FString ULocalCharacterComponent::BuildPromptWithHistory(const FLocalTalkerCharacterConfig& Config, const FString& UserText) const
{
    FString P = FString::Printf(TEXT("System: You are %s. %s %s\n\n"), *GetSpeakerNameResolved(), *Directions, *Desc);
    
    TArray<FLocalTalkMessage> FullHistory;
    if (UWorld* W = GetWorld())
    {
        if (auto* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            FullHistory = Sub->GetContextHistory(const_cast<ULocalCharacterComponent*>(this));
        }
    }

    for (const auto& M : FullHistory)
    {
        P += FString::Printf(TEXT("%s: %s\n"), *M.SpeakerName, *M.Content);
    }

    // Append the instruction or the direct prompt if it's not already the last item in history
    if (!UserText.IsEmpty())
    {
        if (UserText.StartsWith(TEXT("Respond to ")))
        {
            P += FString::Printf(TEXT("\n(Instruction: %s)\n"), *UserText);
        }
        else if (!UserText.StartsWith(TEXT("RAW:")))
        {
            // Only add if not already in history
            bool bInHistory = false;
            if (FullHistory.Num() > 0 && FullHistory.Last().Content == UserText)
            {
                bInHistory = true;
            }

            if (!bInHistory)
            {
                P += FString::Printf(TEXT("User: %s\n"), *UserText);
            }
        }
    }

    P += TEXT("Assistant: ");
    return P;
}

void ULocalCharacterComponent::EmitSubtitle(const FString& Text)
{
    OnSubtitle.Broadcast(GetSpeakerNameResolved(), Text);
    OnSubtitleNative.Broadcast(GetSpeakerNameResolved(), Text);
    if (bShowOnScreenSubtitles && GEngine)
    {
        GEngine->AddOnScreenDebugMessage(-1, 4.0f, FColor::White, FString::Printf(TEXT("%s: %s"), *GetSpeakerNameResolved(), *Text));
    }
}

static FString LocalTalkerPluginBaseDir()
{
    if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LocalTalker")))
    {
        return Plugin->GetBaseDir();
    }
    return FString();
}

FLocalTalkerRuntimePaths ULocalCharacterComponent::ResolvePaths() const
{
    FLocalTalkerRuntimePaths Out = PathsOverride;

    if (bUseProjectSettingsPaths)
    {
        if (const ULocalTalkerSettings* S = GetDefault<ULocalTalkerSettings>())
        {
            Out = S->DefaultPaths;
        }
    }

    // Overlay any explicit overrides (treat empty strings as "no override").
    if (!PathsOverride.LlamaModelPath.IsEmpty()) Out.LlamaModelPath = PathsOverride.LlamaModelPath;
    if (!PathsOverride.LlamaLibPath.IsEmpty()) Out.LlamaLibPath = PathsOverride.LlamaLibPath;
    if (!PathsOverride.PiperExePath.IsEmpty()) Out.PiperExePath = PathsOverride.PiperExePath;
    if (!PathsOverride.PiperVoiceModelPath.IsEmpty()) Out.PiperVoiceModelPath = PathsOverride.PiperVoiceModelPath;
    if (!PathsOverride.WorkingDir.IsEmpty()) Out.WorkingDir = PathsOverride.WorkingDir;

    // Convenience defaults: if the project settings didn't specify piper paths, try the plugin bundle.
    const FString BaseDir = LocalTalkerPluginBaseDir();
    if (!BaseDir.IsEmpty())
    {
        // llama.cpp defaults (only if bundled files exist)
        if (Out.LlamaLibPath.IsEmpty())
        {
            const FString Candidate = FPaths::Combine(BaseDir, TEXT("ThirdParty/llama/Win64/Release/libllama.dll"));
            if (FPaths::FileExists(Candidate))
            {
                Out.LlamaLibPath = Candidate;
            }
        }
        if (Out.LlamaModelPath.IsEmpty())
        {
            // Default bundled model (small) if present.
            const FString Candidate = FPaths::Combine(BaseDir, TEXT("Resources/Models/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"));
            if (FPaths::FileExists(Candidate))
            {
                Out.LlamaModelPath = Candidate;
            }
        }

        if (Out.PiperExePath.IsEmpty())
        {
            Out.PiperExePath = FPaths::Combine(BaseDir, TEXT("ThirdParty/piper/Win64/Release/piper.exe"));
        }
        if (Out.PiperVoiceModelPath.IsEmpty())
        {
            Out.PiperVoiceModelPath = FPaths::Combine(BaseDir, TEXT("Resources/Voices/en_US-lessac-small.onnx"));
        }
        if (Out.WorkingDir.IsEmpty())
        {
            Out.WorkingDir = FPaths::GetPath(Out.PiperExePath);
        }
    }

    return Out;
}

FLocalTalkerCharacterConfig ULocalCharacterComponent::ResolveConfig() const
{
    FLocalTalkerCharacterConfig Out = CharacterConfigOverride;

    if (bUseProjectSettingsConfig)
    {
        if (const ULocalTalkerSettings* S = GetDefault<ULocalTalkerSettings>())
        {
            Out = S->DefaultCharacterConfig;
        }
    }

    // Overlay explicit overrides (heuristic: only override if non-default-ish / non-empty).
    if (!CharacterConfigOverride.Directions.IsEmpty()) Out.Directions = CharacterConfigOverride.Directions;
    if (!CharacterConfigOverride.CharacterDescription.IsEmpty()) Out.CharacterDescription = CharacterConfigOverride.CharacterDescription;
    if (!CharacterConfigOverride.SystemPrompt.IsEmpty()) Out.SystemPrompt = CharacterConfigOverride.SystemPrompt;
    if (!CharacterConfigOverride.Persona.IsEmpty()) Out.Persona = CharacterConfigOverride.Persona;
    if (!CharacterConfigOverride.Stop.IsEmpty()) Out.Stop = CharacterConfigOverride.Stop;

    if (CharacterConfigOverride.MaxContextChars != 0) Out.MaxContextChars = CharacterConfigOverride.MaxContextChars;
    if (CharacterConfigOverride.MaxHistoryMessages != 0) Out.MaxHistoryMessages = CharacterConfigOverride.MaxHistoryMessages;
    if (CharacterConfigOverride.GpuLayers != 0) Out.GpuLayers = CharacterConfigOverride.GpuLayers;
    if (CharacterConfigOverride.MaxTokens != 0) Out.MaxTokens = CharacterConfigOverride.MaxTokens;
    if (CharacterConfigOverride.Temperature != 0.0f) Out.Temperature = CharacterConfigOverride.Temperature;
    if (CharacterConfigOverride.Seed != 0) Out.Seed = CharacterConfigOverride.Seed;

    Out.GpuBackend = CharacterConfigOverride.GpuBackend;
    Out.bSpeak = CharacterConfigOverride.bSpeak;
    Out.bStreamTokens = CharacterConfigOverride.bStreamTokens;

    return Out;
}

// --- TTS Worker ---

class FLocalTalkerTTSWorker : public FRunnable
{
    ULocalCharacterComponent* Owner;
    FLocalTalkerRuntimePaths Paths;
public:
    FLocalTalkerTTSWorker(ULocalCharacterComponent* InOwner, const FLocalTalkerRuntimePaths& InPaths) : Owner(InOwner), Paths(InPaths) {}
    virtual uint32 Run() override
    {
        while (!Owner->bTTSStop)
        {
            FString Sentence;
            if (Owner->SentenceQueue.Dequeue(Sentence))
            {
                FString Err;
                Owner->RunPiperSentenceToAudioQueue(Sentence, Paths, Err);
                if (!Err.IsEmpty())
                {
                    UE_LOG(LogLocalTalker, Warning, TEXT("[%s] Piper/TTS error: %s"), *Owner->GetSpeakerNameResolved(), *Err);
                }
                Owner->PendingSentenceCount.Decrement();
            }
            FPlatformProcess::Sleep(0.01f);
        }
        return 0;
    }
};

void ULocalCharacterComponent::StartTTSWorker(const FLocalTalkerRuntimePaths& Paths)
{
    StopTTSWorker();
    bTTSStop = false;
    TTSRunnable = new FLocalTalkerTTSWorker(this, Paths);
    TTSThread = FRunnableThread::Create(TTSRunnable, TEXT("LocalTalkerTTS"));
}

void ULocalCharacterComponent::StopTTSWorker()
{
    bTTSStop = true;
    if (TTSThread) { TTSThread->WaitForCompletion(); delete TTSThread; TTSThread = nullptr; }
    if (TTSRunnable) { delete TTSRunnable; TTSRunnable = nullptr; }
}

void ULocalCharacterComponent::RunPiperSentenceToAudioQueue(const FString& Sentence, const FLocalTalkerRuntimePaths& Paths, FString& OutErr)
{
    OutErr.Reset();

    const FString PiperExe = Paths.PiperExePath;
    const FString VoiceModel = Paths.PiperVoiceModelPath;

    if (PiperExe.IsEmpty() || !FPaths::FileExists(PiperExe))
    {
        OutErr = FString::Printf(TEXT("PiperExePath missing/invalid: %s"), *PiperExe);
        return;
    }
    if (VoiceModel.IsEmpty() || !FPaths::FileExists(VoiceModel))
    {
        OutErr = FString::Printf(TEXT("PiperVoiceModelPath missing/invalid: %s"), *VoiceModel);
        return;
    }

    // Write output wav to Saved/LocalTalker/tts/<guid>.wav
    const FString OutDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("LocalTalker"), TEXT("tts"));
    IFileManager::Get().MakeDirectory(*OutDir, true);
    const FString WavPath = FPaths::Combine(OutDir, FString::Printf(TEXT("%s.wav"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));

    // Piper usage (common): echo "text" | piper --model <voice.onnx> --output_file <out.wav>
    const FString Args = FString::Printf(TEXT("--model \"%s\" --output_file \"%s\""), *VoiceModel, *WavPath);

    FProcHandle Handle;
    FLocalProcPipes Pipes;
    FString SpawnErr;
    if (!FLocalTalkerProcess::SpawnWithPipes(PiperExe, Args, Paths.WorkingDir, Handle, Pipes, SpawnErr))
    {
        OutErr = SpawnErr;
        return;
    }

    FString StdErrAccum;
    auto OnOut = [](const FString& /*Out*/) {};
    auto OnErr = [&StdErrAccum](const FString& Err) { StdErrAccum += Err; };

    // Send sentence and close stdin so piper exits.
    const FString Line = Sentence + TEXT("\n");
    if (!FLocalTalkerProcess::WriteStdin(Pipes, Line))
    {
        StdErrAccum += TEXT("Failed to write to Piper stdin.\n");
    }
    // Close stdin pipes explicitly to signal EOF.
    if (Pipes.ReadInPipe || Pipes.WriteInPipe)
    {
        FPlatformProcess::ClosePipe(Pipes.ReadInPipe, Pipes.WriteInPipe);
        Pipes.ReadInPipe = nullptr;
        Pipes.WriteInPipe = nullptr;
    }

    FLocalTalkerProcess::PumpOutputUntilExit(Handle, Pipes, OnOut, OnErr, 0.01);
    FPlatformProcess::WaitForProc(Handle);
    FPlatformProcess::CloseProc(Handle);
    FLocalTalkerProcess::ClosePipes(Pipes);

    if (!StdErrAccum.IsEmpty())
    {
        // Don't fail purely on stderr, but surface it if we also fail to load wav.
        StdErrAccum = StdErrAccum.TrimStartAndEnd();
    }

    if (!FPaths::FileExists(WavPath))
    {
        OutErr = FString::Printf(TEXT("Piper did not produce wav. %s"), *StdErrAccum);
        return;
    }

    FLocalWavPcm16 Wav;
    FString WavErr;
    if (!FLocalTalkerWav::LoadWavPcm16(WavPath, Wav, WavErr))
    {
        OutErr = FString::Printf(TEXT("Failed to load wav '%s': %s. %s"), *WavPath, *WavErr, *StdErrAccum);
        return;
    }

    // Convert PCM16 samples to bytes for USoundWaveProcedural::QueueAudio
    FAudioChunk Chunk;
    Chunk.SampleRate = Wav.SampleRate;
    Chunk.NumChannels = Wav.NumChannels;
    Chunk.Bytes.SetNumUninitialized(Wav.Samples.Num() * sizeof(int16));
    FMemory::Memcpy(Chunk.Bytes.GetData(), Wav.Samples.GetData(), Chunk.Bytes.Num());

    PendingAudioChunkCount.Increment();
    AudioQueue.Enqueue(MoveTemp(Chunk));
    LastAudioEnqueueCycles.Store(FPlatformTime::Cycles64());

    // Best-effort cleanup
    IFileManager::Get().Delete(*WavPath, false, true, true);
}
