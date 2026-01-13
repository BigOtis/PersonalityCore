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
#include "Sound/SoundAttenuation.h"

static FString LocalTalkerTimePrefix(const UObject* Obj)
{
    const UWorld* W = Obj ? Obj->GetWorld() : nullptr;
    if (!W)
    {
        return TEXT("");
    }
    return FString::Printf(TEXT("[t=%.2f] "), W->GetTimeSeconds());
}


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
    UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] LocalTalker paths: LlamaLib='%s' Model='%s' PiperExe='%s' Voice='%s' WorkDir='%s'"),
        *LocalTalkerTimePrefix(this),
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

void ULocalCharacterComponent::EmitSubtitle(const FString& Text, float DurationSeconds)
{
    const FString Speaker = GetSpeakerNameResolved();
    OnSubtitle.Broadcast(Speaker, Text);

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

    // PendingSentenceCount represents sentences that still need TTS completion (including in-flight piper work).
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
    UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Enqueued sentence (%d chars)"), *LocalTalkerTimePrefix(this), *GetSpeakerNameResolved(), S.Len());
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
            Owner->RunPiperSentenceToAudio(Sentence, Paths, Err);
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
    FThreadSafeBool& bStop;
    ULocalCharacterComponent* Owner = nullptr;
    FLocalTalkerRuntimePaths Paths;
};

void ULocalCharacterComponent::StartTTSWorker(const FLocalTalkerRuntimePaths& Paths)
{
    if (bTTSWorkerRunning) return;

    bTTSWorkerRunning = true;
    bTTSStop = false;

    TTSRunnable = new FLocalTalkerTTSWorker(SentenceQueue, bTTSStop, this, Paths);
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

void ULocalCharacterComponent::RunPiperSentenceToAudio(const FString& Sentence, const FLocalTalkerRuntimePaths& Paths, FString& OutErr)
{
    TArray<uint8> Bytes;
    int32 SampleRate = 0;
    int32 NumChannels = 0;

    if (!GeneratePiperAudioBytes(Sentence, Paths, Bytes, SampleRate, NumChannels, OutErr))
    {
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
        if (!AudioComp) return;

        USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(this, TEXT("LocalTalkerProcWave"));
        Wave->bLooping = false;
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

        TryStartPendingAudio();
    });

    UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Piper ok: %d bytes, %d ch, %d Hz (ready)"),
        *LocalTalkerTimePrefix(this),
        *GetSpeakerNameResolved(),
        QueuedBytes,
        NumChannels,
        SampleRate
    );
}

bool ULocalCharacterComponent::GeneratePiperAudioBytes(const FString& Sentence, const FLocalTalkerRuntimePaths& Paths, TArray<uint8>& OutBytes, int32& OutSampleRate, int32& OutNumChannels, FString& OutErr)
{
    const FString VoicePath = ResolveVoiceOnnxPath();
    if (Paths.PiperExePath.IsEmpty() || VoicePath.IsEmpty())
    {
        OutErr = TEXT("Piper paths not set. Configure Project Settings -> LocalTalker (PiperExePath + Voices).");
        return false;
    }

    UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Piper start: %s"), *LocalTalkerTimePrefix(this), *GetSpeakerNameResolved(), *Sentence);

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
        UE_LOG(LogLocalTalker, Error, TEXT("%s[%s] Piper spawn failed: %s"), *LocalTalkerTimePrefix(this), *GetSpeakerNameResolved(), *OutErr);
        return false;
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
        UE_LOG(LogLocalTalker, Error, TEXT("%s[%s] %s"), *LocalTalkerTimePrefix(this), *GetSpeakerNameResolved(), *OutErr);
        return false;
    }

    FLocalWavPcm16 W;
    FString WavErr;
    if (!FLocalTalkerWav::LoadWavPcm16(OutWav, W, WavErr))
    {
        OutErr = WavErr;
        UE_LOG(LogLocalTalker, Error, TEXT("%s[%s] WAV load failed: %s"), *LocalTalkerTimePrefix(this), *GetSpeakerNameResolved(), *OutErr);
        return false;
    }

    if (W.Samples.Num() == 0)
    {
        OutErr = TEXT("Piper produced an empty WAV.");
        return false;
    }

    // Clean up the temp file ASAP; we have the audio in memory now.
    PF.DeleteFile(*OutWav);

    // Piper voices are typically mono, but handle basic stereo->mono downmix if needed.
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
    return true;
}

void ULocalCharacterComponent::TryStartPendingAudio()
{
    if (bInterrupted) return;
    if (!PendingAudioWave) return;

    EnsureAudio();
    if (!AudioComp) return;
    if (AudioComp->IsPlaying()) return;
    if (IsAudioBlockedByOtherSpeaker()) return;

    AudioComp->SetSound(PendingAudioWave);
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
    UE_LOG(LogLocalTalker, Log, TEXT("%s[%s] Audio stopped (completed)."), *LocalTalkerTimePrefix(this), *GetSpeakerNameResolved());
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
