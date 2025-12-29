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
#include "Misc/FileHelper.h"
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
    // Avoid treating '\n' as a terminator: it causes JSON-like outputs to get split into stray `"}"` chunks.
    return C == TEXT('.') || C == TEXT('!') || C == TEXT('?');
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

    PumpAudioToProcedural();
    UpdateAudioCompletion();

    // Notify the Director when this turn is truly finished (LLM done + TTS jobs done + audio finished).
    // IMPORTANT: we must not ReleaseTurn while Piper is still generating audio (queues can look empty briefly).
    if (!bNotifiedSubsystemFinished &&
        bLLMFinished &&
        PendingSentenceCount.GetValue() == 0 &&
        PendingAudioChunkCount.GetValue() == 0 &&
        bAudioQueueDrained &&
        (!AudioComp || !AudioComp->IsPlaying()))
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
            const FString Base = FString(FPathViews::GetCleanFilename(Path));
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

static FString LocalTalkerCleanSpokenText(const FString& In)
{
    FString S = TrimSentence(In);
    if (S.IsEmpty()) return S;

    LocalTalkerStripControlTokens(S);

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
    S.TrimStartAndEndInline();
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

    // ---- System ----
    FString SystemBlock;
    SystemBlock += FString::Printf(TEXT("You are %s.\n\n"), *SelfSpeakerName);
    if (!SelfSpeakerName.IsEmpty())
    {
        SystemBlock += FString::Printf(TEXT("You are speaking now as %s. Only output %s's spoken dialogue.\n\n"), *SelfSpeakerName, *SelfSpeakerName);
    }

    // Cast briefs for everyone in the conversation (participants + speakers in history).
    {
        TSet<FString> Seen;
        TArray<FString> Speakers;
        // Keep a stable order: self first, then context participants, then speakers in first-appearance order.
        if (!SelfSpeakerName.IsEmpty())
        {
            Seen.Add(SelfSpeakerName);
            Speakers.Add(SelfSpeakerName);
        }
        for (ULocalCharacterComponent* P : ContextParticipants)
        {
            if (!P) continue;
            const FString Name = P->GetSpeakerNameResolved();
            if (Name.IsEmpty()) continue;
            if (!Seen.Contains(Name))
            {
                Seen.Add(Name);
                Speakers.Add(Name);
            }
        }
        for (const FLocalTalkMessage& M : ContextHistory)
        {
            if (!M.SpeakerName.IsEmpty())
            {
                if (!Seen.Contains(M.SpeakerName))
                {
                    Seen.Add(M.SpeakerName);
                    Speakers.Add(M.SpeakerName);
                }
            }
        }

        if (Speakers.Num() > 0)
        {
            SystemBlock += TEXT("CAST BRIEFS (short):\n");
            for (const FString& Name : Speakers)
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
                if (Brief.IsEmpty()) Brief = TEXT("(no brief provided)");
                SystemBlock += FString::Printf(TEXT("- %s: %s\n"), *Name, *Brief);
            }
            SystemBlock += TEXT("\n");
        }
    }

    if (!C.Directions.IsEmpty() || !C.SystemPrompt.IsEmpty())
    {
        const FString Directions = !C.Directions.IsEmpty() ? C.Directions : C.SystemPrompt;
        if (!Directions.IsEmpty())
        {
            SystemBlock += TEXT("DIRECTIONS:\n");
            SystemBlock += Directions.TrimStartAndEnd();
            SystemBlock += TEXT("\n\n");
        }
    }

    if (!C.CharacterDescription.IsEmpty() || !C.Persona.IsEmpty())
    {
        const FString Desc = !C.CharacterDescription.IsEmpty() ? C.CharacterDescription : C.Persona;
        if (!Desc.IsEmpty())
        {
            SystemBlock += TEXT("CHARACTER:\n");
            SystemBlock += Desc.TrimStartAndEnd();
            SystemBlock += TEXT("\n\n");
        }
    }

    SystemBlock +=
        TEXT("RULES:\n")
        TEXT("- Stay strictly in character at all times.\n")
        TEXT("- Keep continuity with the conversation.\n")
        TEXT("- Always move the conversation forward; add a NEW concrete detail or viewpoint.\n")
        TEXT("- Do not echo the last line verbatim.\n")
        TEXT("- Output only the spoken dialogue (no speaker labels, no transcripts). Never output lines like \"User:\", \"Assistant:\", or \"Name:\".\n")
        TEXT("- Do not refer to yourself as Assistant, AI, or a language model.\n")
        TEXT("- Do not output bracketed speaker tags like \"[Milo]\" or \"(Otis)\".\n")
        TEXT("- Do not output any control tokens or markup.\n")
        TEXT("- Never restate the cast briefs, directions, or character descriptions; they are reference only.\n")
        TEXT("- Do not describe yourself or your role; just speak as if in the scene.\n")
        TEXT("- If unsure, ask a short, in-character question about the current situation.\n")
        TEXT("- Speak in 2-4 complete sentences unless asked for shorter.\n");

    // ---- History ----
    // Prefer last N messages and also respect MaxContextChars (approx, content-only) so prompts don't balloon.
    const int32 MaxMsgs = (C.MaxHistoryMessages > 0) ? C.MaxHistoryMessages : ContextHistory.Num();
    int32 StartIdx = 0;
    if (ContextHistory.Num() > MaxMsgs)
    {
        StartIdx = ContextHistory.Num() - MaxMsgs;
    }
    const int32 MaxChars = (C.MaxContextChars > 0) ? C.MaxContextChars : 1600;
    if (MaxChars > 0)
    {
        // Trim oldest messages until the content-only budget fits (speaker + content).
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

    FString Prompt;
    Prompt.Reserve(SystemBlock.Len() + 2048);
    Prompt += TEXT("<|begin_of_text|>");
    AppendTurn(Prompt, TEXT("system"), SystemBlock);

    // Consolidate conversation history into a single transcript block to avoid role confusion.
    FString Transcript;
    Transcript += TEXT("CONVERSATION SO FAR (most recent last):\n");
    bool bHasHistory = false;
    for (int32 i = StartIdx; i < ContextHistory.Num(); i++)
    {
        const FLocalTalkMessage& M = ContextHistory[i];
        const FString Clean = LocalTalkerOneLine(M.Content);
        if (Clean.IsEmpty()) continue;

        const FString Speaker = M.bFromUser ? TEXT("Player") : (M.SpeakerName.IsEmpty() ? TEXT("Unknown") : M.SpeakerName);
        Transcript += FString::Printf(TEXT("%s: %s\n"), *Speaker, *Clean);
        bHasHistory = true;
    }
    if (!bHasHistory)
    {
        Transcript += TEXT("(no prior conversation)\n");
    }
    Transcript += TEXT("\n");

    if (!TurnPrompt.IsEmpty() && (TurnPrompt.StartsWith(TEXT("Director instruction:"), ESearchCase::IgnoreCase) || TurnPrompt.StartsWith(TEXT("Respond to "), ESearchCase::IgnoreCase)))
    {
        const FString Clean = LocalTalkerOneLine(TurnPrompt);
        Transcript += FString::Printf(TEXT("DIRECTOR INSTRUCTION (do not repeat): %s\n\n"), *Clean);
    }

    if (!SelfSpeakerName.IsEmpty())
    {
        Transcript += FString::Printf(TEXT("You are now speaking as %s. Write only %s's next spoken line, no name prefix.\n"), *SelfSpeakerName, *SelfSpeakerName);
    }

    AppendTurn(Prompt, TEXT("user"), Transcript);

    // Generation prompt
    Prompt += TEXT("<|start_header_id|>assistant<|end_header_id|>\n\n");
    return Prompt;
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
            Out.LlamaModelPath = FPaths::Combine(Base, TEXT("Resources/Models/Llama-3.2-3B-Q4_K_M.gguf"));
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

    if (Out.Stop.IsEmpty())
    {
        const FLocalTalkerRuntimePaths Paths = ResolvePaths();
        if (LocalTalkerModelLooksLikeLlama3(Paths.LlamaModelPath))
        {
            Out.Stop = TEXT("<|eot_id|>");
        }
    }

    return Out;
}

void ULocalCharacterComponent::Interrupt()
{
    bInterrupted = true;
    bLLMFinished = true;

    if (ActiveLLM)
    {
        ActiveLLM->Cancel();
    }

    LLMTextBuffer.Reset();
    LLMFullText.Reset();

    FString Tmp;
    while (SentenceQueue.Dequeue(Tmp)) {}

    TArray<uint8> Buf;
    while (AudioQueue.Dequeue(Buf)) {}

    PendingSentenceCount.Reset();
    PendingAudioChunkCount.Reset();
    bAudioQueueDrained = true;

    if (AudioComp) AudioComp->Stop();
    if (ProcWave) ProcWave->ResetAudio();

    bAudioStarted = false;
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
    bInterrupted = false;
    bLLMFinished = true;

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
    bInterrupted = false;
    bLLMFinished = false;
    bSpokeThisTurn = false;
    bNotifiedSubsystemFinished = false;
    bAudioQueueDrained = false;
    PendingSentenceCount.Reset();
    PendingAudioChunkCount.Reset();

    LLMTextBuffer.Reset();
    LLMFullText.Reset();
    LastTextAppendSeconds = FPlatformTime::Seconds();

    EnsureAudio();

    const FLocalTalkerRuntimePaths Paths = ResolvePaths();
    const FLocalTalkerCharacterConfig Config = ResolveConfig();
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
    if (UWorld* W = GetWorld())
    {
        if (auto* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            const TArray<FLocalTalkMessage> ContextHistory = Sub->GetContextHistory(this);
            const TArray<ULocalCharacterComponent*> Participants = Sub->GetContextParticipants(this);
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

    LocalTalkerDumpPromptToFile(GetSpeakerNameResolved(), PromptText);

    ActiveLLM = ULocalTalkerInProcGenerateAsync::GenerateStreamingInProcWithPromptText(this, Paths, Config, PromptText);
    ActiveLLM->OnToken.AddDynamic(this, &ULocalCharacterComponent::HandleLLMToken);
    ActiveLLM->OnDelta.AddDynamic(this, &ULocalCharacterComponent::HandleLLMDelta);
    ActiveLLM->OnCompleted.AddDynamic(this, &ULocalCharacterComponent::HandleLLMCompleted);
    ActiveLLM->OnError.AddDynamic(this, &ULocalCharacterComponent::HandleLLMError);
    ActiveLLM->Activate();
}

void ULocalCharacterComponent::OnHeardSpeech(const FString& InSpeakerName, const FString& Text, bool bFromUser)
{
    // The Director owns the authoritative context history. This callback is for local reactions / logging.
    FString OneLine = LocalTalkerOneLine(Text);
    const int32 MaxChars = 120;
    if (OneLine.Len() > MaxChars) OneLine = OneLine.Left(MaxChars) + TEXT("...");
    UE_LOG(LogLocalTalker, Verbose, TEXT("[%s] Heard (%s) from %s: %s"),
        *GetSpeakerNameResolved(),
        bFromUser ? TEXT("User") : TEXT("NPC"),
        *InSpeakerName,
        *OneLine
    );
}

void ULocalCharacterComponent::HandleLLMError(const FString& Error)
{
    UE_LOG(LogLocalTalker, Error, TEXT("[%s] LLM error: %s"), *GetSpeakerNameResolved(), *Error);
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
        ExtractAndEnqueueSentences(/*bForceFlush*/ true);
    }
}

void ULocalCharacterComponent::EnqueueSentence(const FString& Sentence)
{
    const FString S = LocalTalkerCleanSpokenText(Sentence);
    if (S.Len() <= 0) return;

    // PendingSentenceCount represents sentences that still need TTS completion (including in-flight piper work).
    PendingSentenceCount.Increment();

    SentenceQueue.Enqueue(S);
    EmitSubtitle(S);
    bSpokeThisTurn = true;
    if (UWorld* W = GetWorld())
    {
        if (auto* Sub = W->GetSubsystem<ULocalTalkConversationSubsystem>())
        {
            Sub->BroadcastSentence(this, S, /*bFromUser*/ false);
        }
    }
    UE_LOG(LogLocalTalker, Log, TEXT("[%s] Enqueued sentence (%d chars)"), *GetSpeakerNameResolved(), S.Len());
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

            FString Err;
            Owner->RunPiperSentenceToAudioQueue(Sentence, Paths, Err);
            if (!Err.IsEmpty())
            {
                AsyncTask(ENamedThreads::GameThread, [Owner = Owner, Err]()
                {
                    if (Owner) Owner->OnError.Broadcast(Err);
                });
            }

            // Mark this sentence as fully processed (either produced audio or errored).
            Owner->PendingSentenceCount.Decrement();
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

    // Ensure procedural wave format matches.
    AsyncTask(ENamedThreads::GameThread, [this, SampleRate, NumChannels]()
    {
        EnsureAudio();
        EnsureProcWaveFormat(SampleRate, NumChannels);
    });

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

    TArray<uint8> Bytes;
    Bytes.SetNumUninitialized(Use.Num() * sizeof(int16));
    FMemory::Memcpy(Bytes.GetData(), Use.GetData(), Bytes.Num());

    PendingAudioChunkCount.Increment();
    AudioQueue.Enqueue(MoveTemp(Bytes));
    bAudioQueueDrained = false;

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

    TArray<uint8> Bytes;
    bool bQueued = false;

    for (int32 i = 0; i < 8; i++)
    {
        if (!AudioQueue.Dequeue(Bytes)) break;
        if (Bytes.Num() == 0) continue;

        ProcWave->QueueAudio(Bytes.GetData(), Bytes.Num());
        bQueued = true;
        PendingAudioChunkCount.Decrement();

        if (UWorld* W = GetWorld())
        {
            const int32 SampleRate = ProcWave->GetSampleRateForCurrentPlatform();
            const int32 NumChannels = FMath::Max(1, ProcWave->NumChannels);
            const double Duration = (SampleRate > 0)
                ? (static_cast<double>(Bytes.Num()) / (2.0 * NumChannels * SampleRate))
                : 0.0;
            const double Now = W->GetTimeSeconds();
            const double Base = FMath::Max(EstimatedAudioEndWorldSeconds, Now);
            EstimatedAudioEndWorldSeconds = Base + Duration;
        }
    }

    if (bQueued && !bAudioStarted)
    {
        AudioComp->SetSound(ProcWave);
        AudioComp->Play();
        bAudioStarted = true;
        UE_LOG(LogLocalTalker, Log, TEXT("[%s] Audio started (procedural)."), *GetSpeakerNameResolved());
    }

    if (AudioQueue.IsEmpty() && PendingAudioChunkCount.GetValue() == 0)
    {
        bAudioQueueDrained = true;
    }
}

void ULocalCharacterComponent::UpdateAudioCompletion()
{
    if (!AudioComp || !ProcWave) return;
    if (!bAudioStarted) return;
    if (!bAudioQueueDrained) return;
    if (!bLLMFinished) return;

    if (!AudioComp->IsPlaying())
    {
        EstimatedAudioEndWorldSeconds = 0.0;
        bAudioStarted = false;
        return;
    }

    if (UWorld* W = GetWorld())
    {
        const double Now = W->GetTimeSeconds();
        if (EstimatedAudioEndWorldSeconds > 0.0 &&
            Now >= (EstimatedAudioEndWorldSeconds + TurnReleaseAudioTailSeconds))
        {
            AudioComp->Stop();
            bAudioStarted = false;
            EstimatedAudioEndWorldSeconds = 0.0;
        }
    }
}
