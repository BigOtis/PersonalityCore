#include "LocalTalkerInProcAsync.h"
#include "LocalLlamaDyn.h"
#include "LocalTalkerLlamaCache.h"
#include "LocalTalkerLog.h"
#include "LocalTalkerSettings.h"

#include "Async/Async.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"

static TAutoConsoleVariable<int32> CVarLocalTalkerTraceConversation_InProc(
    TEXT("LocalTalker.TraceConversation"),
    0,
    TEXT("Enable high-signal conversation tracing logs for LocalTalker.\n")
    TEXT("0 = off (default)\n")
    TEXT("1 = on"),
    ECVF_Default
);

static TAutoConsoleVariable<int32> CVarLocalTalkerLogLLMPerf(
    TEXT("LocalTalker.LogLLMPerf"),
    1,
    TEXT("Log LLM timing breakdown per generation.\n")
    TEXT("0 = off\n")
    TEXT("1 = on (default)"),
    ECVF_Default
);

static FString LocalTalkerOneLineTrunc(const FString& In, int32 MaxChars)
{
    FString S = In;
    S.ReplaceInline(TEXT("\r"), TEXT(" "));
    S.ReplaceInline(TEXT("\n"), TEXT(" "));
    S.TrimStartAndEndInline();
    if (MaxChars > 0 && S.Len() > MaxChars)
    {
        S = S.Left(MaxChars) + TEXT("…");
    }
    return S;
}

static FString LocalTalkerNormalizeTokenPiece(FString Piece)
{
    // Some tokenizers use U+2581 (LOWER ONE EIGHTH BLOCK) as a visible "space" marker.
    // Convert it to a real space.
    Piece.ReplaceInline(TEXT("\u2581"), TEXT(" "));
    return Piece;
}

static bool LocalTalkerShouldStopEarly(const FString& FullOut, bool bTaggedPrompt)
{
    // Deterministic "dialogue mode" stopping rules:
    // - Stop at first newline
    // - Stop after a reasonable one-line length
    // - Prefer to end on punctuation once the line is already fairly long (prevents rambling)
    if (!bTaggedPrompt && (FullOut.Contains(TEXT("\n")) || FullOut.Contains(TEXT("\r"))))
    {
        return true;
    }

    // Hard cap: keep it a single line suitable for speech.
    if (FullOut.Len() >= 480)
    {
        return true;
    }

    // If we already have a meaningful chunk, allow longer sentences, but still try to end cleanly.
    if (FullOut.Len() >= 200)
    {
        const TCHAR Last = FullOut[FullOut.Len() - 1];
        if (Last == '.' || Last == '!' || Last == '?' )
        {
            return true;
        }
        // If it starts with a quote, stop after closing quote (common for chat models).
        if (FullOut[0] == '"' && Last == '"')
        {
            return true;
        }
    }

    return false;
}

ULocalTalkerInProcGenerateAsync* ULocalTalkerInProcGenerateAsync::GenerateStreamingInProc(
    UObject* WorldContextObject,
    const FLocalTalkerRuntimePaths& InPaths,
    const FLocalTalkerCharacterConfig& InCharacter,
    const FString& InUserPrompt
)
{
    ULocalTalkerInProcGenerateAsync* Node = NewObject<ULocalTalkerInProcGenerateAsync>();
    Node->WorldContextObject = WorldContextObject;
    Node->Paths = InPaths;
    Node->Character = InCharacter;
    Node->UserPrompt = InUserPrompt;
    Node->AddToRoot();
    return Node;
}

ULocalTalkerInProcGenerateAsync* ULocalTalkerInProcGenerateAsync::GenerateStreamingInProcWithPromptText(
    UObject* WorldContextObject,
    const FLocalTalkerRuntimePaths& InPaths,
    const FLocalTalkerCharacterConfig& InCharacter,
    const FString& InPromptText
)
{
    ULocalTalkerInProcGenerateAsync* Node = NewObject<ULocalTalkerInProcGenerateAsync>();
    Node->WorldContextObject = WorldContextObject;
    Node->Paths = InPaths;
    Node->Character = InCharacter;
    Node->PromptText = InPromptText;
    Node->bPromptIsFull = true;
    Node->AddToRoot();
    return Node;
}

void ULocalTalkerInProcGenerateAsync::Cancel()
{
    bCancel = true;
}

static bool LocalTalkerAbortCb(void* Data)
{
    const FThreadSafeBool* Flag = static_cast<const FThreadSafeBool*>(Data);
    // FThreadSafeBool privately inherits FThreadSafeCounter in UE 5.5; use its bool conversion.
    return Flag && (*Flag);
}

static bool LocalTalkerModelLooksLikeLlama3Async(const FString& ModelPath)
{
    const FString Base = FPaths::GetBaseFilename(ModelPath).ToLower();
    return Base.Contains(TEXT("llama-3")) || Base.Contains(TEXT("llama3"));
}

static FString BuildPrompt(const FLocalTalkerRuntimePaths& P, const FLocalTalkerCharacterConfig& C, const FString& UserPrompt)
{
    // Build a prompt matching the expected chat template for the model family we're running.
    // The biggest lever to get stable formatting/spacing is matching the training template.

    const FString Directions = !C.Directions.IsEmpty() ? C.Directions : C.SystemPrompt;
    const FString Desc = !C.CharacterDescription.IsEmpty() ? C.CharacterDescription : C.Persona;

    const FString SpeakerTag = TEXT("SPEAKER");

    FString SystemBlock;
    SystemBlock += TEXT("You are the current speaker.\n\n");
    SystemBlock += TEXT("You must output EXACTLY ONE message from the speaker and nothing else.\n\n");
    SystemBlock += TEXT("Output format (must match exactly):\n");
    SystemBlock += FString::Printf(TEXT("[%s] <dialogue> [/%s]\n\n"), *SpeakerTag, *SpeakerTag);
    SystemBlock += TEXT("Rules:\n");
    SystemBlock += FString::Printf(TEXT("- Your reply MUST begin with [%s] and end with [/%s].\n"), *SpeakerTag, *SpeakerTag);
    SystemBlock += TEXT("- Output only that single tagged block. No extra text before or after.\n");
    SystemBlock += FString::Printf(TEXT("- Do not output any other tags besides [%s] ... [/%s].\n"), *SpeakerTag, *SpeakerTag);
    SystemBlock += TEXT("- No narration, no actions, no stage directions.\n");
    SystemBlock += TEXT("- 1-3 sentences, natural and specific.\n");
    SystemBlock += TEXT("- Include exactly ONE concrete detail from the scene or context.\n");
    SystemBlock += TEXT("- Do not repeat or paraphrase the last line.\n");
    SystemBlock += TEXT("- Do not reuse any full sentence from the transcript.\n");
    SystemBlock += TEXT("- Do not reuse any 5+ word sequence from the transcript.\n");
    SystemBlock += TEXT("- If your draft matches any earlier line, discard it and write a different reply.\n");
    SystemBlock += TEXT("- Do not ask the same question twice; ask a new question with new wording.\n");

    // Keep system strictly for role + output rules. Character details belong in the user block.

    if (LocalTalkerModelLooksLikeLlama3Async(P.LlamaModelPath))
    {
        FString Prompt;
        Prompt.Reserve(SystemBlock.Len() + UserPrompt.Len() + 128);
        Prompt += TEXT("<|begin_of_text|>");
        Prompt += TEXT("<|start_header_id|>system<|end_header_id|>\n");
        Prompt += SystemBlock;
        Prompt += TEXT("\n<|eot_id|>\n");
        Prompt += TEXT("<|start_header_id|>user<|end_header_id|>\n");
        Prompt += TEXT("Character Descriptions:\n");
        Prompt += TEXT("- Speaker: ");
        Prompt += Desc.IsEmpty() ? TEXT("(no description provided)") : Desc.TrimStartAndEnd();
        Prompt += TEXT("\n\n");
        Prompt += TEXT("[TRANSCRIPT]\n");
        Prompt += TEXT("[/TRANSCRIPT]\n\n");
        Prompt += TEXT("No transcript yet. Say a brief greeting to start the conversation.\n\n");
        Prompt += TEXT("Next speaker must be [SPEAKER].\n");
        Prompt += TEXT("What would the speaker say next? Output only in the required [SPEAKER] ... [/SPEAKER] format.\n");
        Prompt += TEXT("\n<|eot_id|>\n");
        Prompt += TEXT("<|start_header_id|>assistant<|end_header_id|>\n\n");
        return Prompt;
    }

    // Default fallback: Zephyr-style tags (works well for TinyLlama/Zephyr family).
    FString Prompt;
    Prompt.Reserve(SystemBlock.Len() + UserPrompt.Len() + 64);

    Prompt += TEXT("<|system|>\n");
    Prompt += SystemBlock;
    Prompt += TEXT("</s>\n");

    Prompt += TEXT("<|user|>\n");
    Prompt += UserPrompt.TrimStartAndEnd();
    Prompt += TEXT("</s>\n");

    // Generation prompt
    Prompt += TEXT("<|assistant|>\n");
    return Prompt;
}

void ULocalTalkerInProcGenerateAsync::DispatchError(const FString& Msg)
{
    AsyncTask(ENamedThreads::GameThread, [this, Msg]()
    {
        OnError.Broadcast(Msg);
        RemoveFromRoot();
        SetReadyToDestroy();
    });
}

static void DispatchToken(ULocalTalkerInProcGenerateAsync* Self, const FString& TokenText)
{
    AsyncTask(ENamedThreads::GameThread, [Self, TokenText]()
    {
        if (!Self) return;
        Self->OnToken.Broadcast(TokenText);
    });
}

static void DispatchDelta(ULocalTalkerInProcGenerateAsync* Self, const FString& DeltaText)
{
    AsyncTask(ENamedThreads::GameThread, [Self, DeltaText]()
    {
        if (!Self) return;
        Self->OnDelta.Broadcast(DeltaText);
    });
}

static void DispatchCompleted(ULocalTalkerInProcGenerateAsync* Self, const FString& FullText)
{
    AsyncTask(ENamedThreads::GameThread, [Self, FullText]()
    {
        if (!Self) return;
        Self->OnCompleted.Broadcast(FullText);
        Self->RemoveFromRoot();
        Self->SetReadyToDestroy();
    });
}

void ULocalTalkerInProcGenerateAsync::Activate()
{
    const FLocalTalkerRuntimePaths P = Paths;
    const FLocalTalkerCharacterConfig C = Character;
    const FString Prompt = bPromptIsFull ? PromptText : BuildPrompt(P, C, UserPrompt);
    const bool bTaggedPrompt = Prompt.Contains(TEXT("[SPEAKER]")) && Prompt.Contains(TEXT("[/SPEAKER]"));

    const double ActivateStart = FPlatformTime::Seconds();
    if (CVarLocalTalkerTraceConversation_InProc.GetValueOnAnyThread() != 0)
    {
        UE_LOG(
            LogLocalTalker,
            Log,
            TEXT("[TalkTrace][LLM] Activate (in-proc) promptLen=%d promptIsFull=%d maxTokens=%d temp=%.2f stopLen=%d promptPreview='%s'"),
            Prompt.Len(),
            bPromptIsFull ? 1 : 0,
            C.MaxTokens,
            C.Temperature,
            C.Stop.Len(),
            *LocalTalkerOneLineTrunc(Prompt, 220)
        );
    }

    Async(EAsyncExecution::ThreadPool, [this, P, C, Prompt, ActivateStart, bTaggedPrompt]()
    {
        const double Start = FPlatformTime::Seconds();
        double AcquireDone = 0.0;
        double TokenizeDone = 0.0;
        double PromptEvalDone = 0.0;
        double GenStart = 0.0;
        double GenDone = 0.0;
        int32 GeneratedTokens = 0;
        int32 PromptTokenCount = 0;
        int32 PromptTokenCountUsed = 0;
        int32 PromptTokensTrimmed = 0;
        int32 UsedContextTokens = 0;
        if (P.LlamaLibPath.IsEmpty())
        {
            DispatchError(TEXT("LlamaLibPath is empty. Set Project Settings -> LocalTalker -> DefaultPaths.LlamaLibPath"));
            return;
        }
        if (P.LlamaModelPath.IsEmpty())
        {
            DispatchError(TEXT("LlamaModelPath is empty. Set Project Settings -> LocalTalker -> DefaultPaths.LlamaModelPath"));
            return;
        }

        FLocalLlamaApi* Api = nullptr;
        llama_model* Model = nullptr;
        const llama_vocab* Vocab = nullptr;

        {
            FString CacheErr;
            if (!FLocalTalkerLlamaCache::Get().Acquire(P.LlamaLibPath, P.LlamaModelPath, C.GpuLayers, C.GpuBackend, Api, Model, Vocab, CacheErr))
            {
                DispatchError(CacheErr);
                return;
            }
        }
        AcquireDone = FPlatformTime::Seconds();

        if (!Api || !Model || !Vocab)
        {
            DispatchError(TEXT("Internal error: llama shared state not initialized."));
            return;
        }

        // Tokenize prompt
        FTCHARToUTF8 PromptUtf8(*Prompt);
        const int32 PromptLen = PromptUtf8.Length();
        TArray<llama_token> PromptTokens;
        PromptTokens.SetNumZeroed(PromptLen + 64);

        int32_t NPrompt = Api->llama_tokenize(
            Vocab,
            PromptUtf8.Get(),
            PromptLen,
            PromptTokens.GetData(),
            (int32_t)PromptTokens.Num(),
            true,  // add_special
            true   // parse_special
        );

        if (NPrompt < 0)
        {
            // Buffer too small; retry with requested size.
            const int32 Needed = -NPrompt;
            if (Needed <= 0)
            {
                DispatchError(TEXT("llama_tokenize failed (invalid return)."));
                return;
            }

            PromptTokens.SetNumZeroed(Needed);
            NPrompt = Api->llama_tokenize(
                Vocab,
                PromptUtf8.Get(),
                PromptLen,
                PromptTokens.GetData(),
                (int32_t)PromptTokens.Num(),
                true,
                true
            );

            if (NPrompt < 0)
            {
                DispatchError(TEXT("llama_tokenize failed after resizing buffer."));
                return;
            }
        }

        PromptTokens.SetNum(NPrompt);
        PromptTokenCount = PromptTokens.Num();
        TokenizeDone = FPlatformTime::Seconds();

        const ULocalTalkerSettings* S = GetDefault<ULocalTalkerSettings>();
        const int32 MinCtx = S ? S->MinContextTokens : 512;
        const int32 MaxCtx = S ? S->MaxContextTokens : 2048;
        const int32 Margin = S ? S->ContextTokenMargin : 64;
        const int32 NeededCtx = PromptTokens.Num() + C.MaxTokens + Margin;
        UsedContextTokens = FMath::Clamp(NeededCtx, MinCtx, MaxCtx);

        const int32 MaxPromptTokens = FMath::Max(0, UsedContextTokens - C.MaxTokens - 1);
        if (MaxPromptTokens > 0 && PromptTokens.Num() > MaxPromptTokens)
        {
            const int32 Drop = PromptTokens.Num() - MaxPromptTokens;
            PromptTokens.RemoveAt(0, Drop, EAllowShrinking::No);
            PromptTokensTrimmed = Drop;
        }
        PromptTokenCountUsed = PromptTokens.Num();

        // Context
        llama_context_params CParams = Api->llama_context_default_params();
        const int32 Cores = FMath::Max(1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
        CParams.n_threads = Cores;
        CParams.n_threads_batch = Cores;
        CParams.n_ctx = UsedContextTokens;
        CParams.n_batch = FMath::Min(512, UsedContextTokens);
        CParams.abort_callback = &LocalTalkerAbortCb;
        CParams.abort_callback_data = (void*)&bCancel;

        llama_context* Ctx = Api->llama_init_from_model ? Api->llama_init_from_model(Model, CParams)
                                                        : (Api->llama_new_context_with_model ? Api->llama_new_context_with_model(Model, CParams) : nullptr);
        if (!Ctx)
        {
            DispatchError(TEXT("Failed to create llama_context."));
            return;
        }

        if (Api->llama_set_n_threads)
        {
            Api->llama_set_n_threads(Ctx, CParams.n_threads, CParams.n_threads_batch);
        }

        // Evaluate prompt in chunks
        int32 PromptIdx = 0;
        while (PromptIdx < PromptTokens.Num())
        {
            if (bCancel)
            {
                // Model + DLL are owned by FLocalTalkerLlamaCache and stay loaded for the app lifetime.
                Api->llama_free(Ctx);
                DispatchCompleted(this, TEXT(""));
                return;
            }

            const int32 Remaining = PromptTokens.Num() - PromptIdx;
            const int32 Chunk = FMath::Min<int32>(Remaining, (int32)CParams.n_batch);

            llama_batch Batch = Api->llama_batch_get_one(PromptTokens.GetData() + PromptIdx, Chunk);
            const int32_t DecodeRes = Api->llama_decode(Ctx, Batch);
            if (DecodeRes != 0)
            {
                Api->llama_free(Ctx);
                DispatchError(FString::Printf(TEXT("llama_decode(prompt) failed: %d"), (int32)DecodeRes));
                return;
            }

            PromptIdx += Chunk;
        }
        PromptEvalDone = FPlatformTime::Seconds();

        // Sampler chain
        llama_sampler_chain_params SParams = Api->llama_sampler_chain_default_params();
        llama_sampler* Sampler = Api->llama_sampler_chain_init(SParams);
        if (!Sampler)
        {
            Api->llama_free(Ctx);
            DispatchError(TEXT("Failed to init llama sampler chain."));
            return;
        }

        // Sampler chain (configurable best-practice defaults).
        const int32 TopK = FMath::Max(0, C.TopK);
        const float TopP = FMath::Clamp(C.TopP, 0.0f, 1.0f);
        if (TopK > 0)
        {
            Api->llama_sampler_chain_add(Sampler, Api->llama_sampler_init_top_k(TopK));
        }
        if (TopP > 0.0f && TopP < 1.0f)
        {
            Api->llama_sampler_chain_add(Sampler, Api->llama_sampler_init_top_p(TopP, 1));
        }

        // Optional samplers if exported by this llama.cpp build.
        if (Api->llama_sampler_init_min_p)
        {
            const float MinP = FMath::Clamp(C.MinP, 0.0f, 1.0f);
            if (MinP > 0.0f)
            {
                Api->llama_sampler_chain_add(Sampler, Api->llama_sampler_init_min_p(MinP, 1));
            }
        }
        if (Api->llama_sampler_init_typical)
        {
            const float TypicalP = FMath::Clamp(C.TypicalP, 0.0f, 1.0f);
            if (TypicalP > 0.0f && TypicalP < 1.0f)
            {
                Api->llama_sampler_chain_add(Sampler, Api->llama_sampler_init_typical(TypicalP, 1));
            }
        }

        // Repetition penalties (optional) — helps prevent echo loops like "Hola buddy" ping-pong.
        if (Api->llama_sampler_init_penalties)
        {
            const int32 LastN = FMath::Max(0, C.RepeatLastN);
            const float Repeat = FMath::Max(1.0f, C.RepeatPenalty);
            const float Freq = FMath::Max(0.0f, C.FrequencyPenalty);
            const float Pres = FMath::Max(0.0f, C.PresencePenalty);
            if (LastN > 0 && (Repeat > 1.0f || Freq > 0.0f || Pres > 0.0f))
            {
                Api->llama_sampler_chain_add(Sampler, Api->llama_sampler_init_penalties(LastN, Repeat, Freq, Pres));
            }
        }

        const float Temp = FMath::Max(0.0f, C.Temperature);
        if (Temp > 0.0f)
        {
            Api->llama_sampler_chain_add(Sampler, Api->llama_sampler_init_temp(Temp));
            const uint32 Seed = (C.Seed != 0) ? (uint32)C.Seed : (uint32)FPlatformTime::Cycles();
            Api->llama_sampler_chain_add(Sampler, Api->llama_sampler_init_dist(Seed));
        }
        else
        {
            Api->llama_sampler_chain_add(Sampler, Api->llama_sampler_init_greedy());
        }

        FString FullOut;
        TArray<FString> StopSequences = C.StopSequences;
        if (StopSequences.Num() == 0 && !C.Stop.IsEmpty())
        {
            StopSequences.Add(C.Stop);
        }
        if (StopSequences.Num() == 0 && bTaggedPrompt)
        {
            StopSequences.Add(TEXT("<|eot_id|>"));
            StopSequences.Add(TEXT("[/SPEAKER]"));
            StopSequences.Add(TEXT("\n[PLAYER]"));
            StopSequences.Add(TEXT("\n[SPEAKER]"));
            StopSequences.Add(TEXT("\nNext speaker must be"));
            StopSequences.Add(TEXT("\n[TRANSCRIPT]"));
            StopSequences.Add(TEXT("\n[/TRANSCRIPT]"));
            StopSequences.Add(TEXT("\nWhat would "));
            StopSequences.Add(TEXT("\n<|start_header_id|>"));
            StopSequences.Add(TEXT("\n<|begin_of_text|>"));
        }

        GenStart = FPlatformTime::Seconds();
        for (int32 i = 0; i < C.MaxTokens; i++)
        {
            if (bCancel)
            {
                break;
            }

            llama_token Tok = Api->llama_sampler_sample(Sampler, Ctx, -1);
            Api->llama_sampler_accept(Sampler, Tok);
            GeneratedTokens++;

            if (Api->llama_vocab_is_eog(Vocab, Tok))
            {
                break;
            }

            // Convert token -> UTF-8 piece
            FString Piece;
            if (Api->llama_token_to_piece)
            {
                // Preferred: decode exactly one token to its text piece.
                // lstrip=0 keeps intended whitespace; special=false hides control tokens.
                char Tmp[256];
                int32_t Written = Api->llama_token_to_piece(Vocab, Tok, Tmp, (int32_t)sizeof(Tmp), 0, false);
                if (Written >= 0)
                {
                    TArray<char> Buf;
                    if (Written >= (int32_t)sizeof(Tmp))
                    {
                        Buf.SetNumZeroed(Written + 1);
                        const int32_t Written2 = Api->llama_token_to_piece(Vocab, Tok, Buf.GetData(), Written, 0, false);
                        if (Written2 > 0)
                        {
                            Buf[Written2] = '\0';
                            Piece = UTF8_TO_TCHAR(Buf.GetData());
                        }
                    }
                    else
                    {
                        Tmp[Written] = '\0';
                        Piece = UTF8_TO_TCHAR(Tmp);
                    }
                }
            }
            else
            {
                // Fallback: detokenize a single token.
                // remove_special=true strips BOS/EOS; unparse_special=false prevents rendering control tokens.
                char Tmp[256];
                int32_t Written = Api->llama_detokenize(Vocab, &Tok, 1, Tmp, (int32_t)sizeof(Tmp), true, false);
                if (Written >= 0)
                {
                    if (Written < (int32_t)sizeof(Tmp))
                    {
                        Tmp[Written] = '\0';
                        Piece = UTF8_TO_TCHAR(Tmp);
                    }
                }
                else
                {
                    const int32 Need = -Written;
                    TArray<char> Big;
                    Big.SetNumZeroed(Need + 1);
                    int32_t Written2 = Api->llama_detokenize(Vocab, &Tok, 1, Big.GetData(), Need, true, false);
                    if (Written2 > 0)
                    {
                        Big[Written2] = '\0';
                        Piece = UTF8_TO_TCHAR(Big.GetData());
                    }
                }
            }

            if (!Piece.IsEmpty())
            {
                Piece = LocalTalkerNormalizeTokenPiece(MoveTemp(Piece));
                FullOut += Piece;
                DispatchToken(this, Piece);
                DispatchDelta(this, Piece);

                if (LocalTalkerShouldStopEarly(FullOut, bTaggedPrompt))
                {
                    break;
                }

                // Check stop sequences only if configured
                if (StopSequences.Num() > 0)
                {
                    int32 BestIdx = INDEX_NONE;
                    int32 BestLen = 0;
                    for (const FString& Stop : StopSequences)
                    {
                        if (Stop.IsEmpty()) continue;
                        const int32 Idx = FullOut.Find(Stop);
                        if (Idx != INDEX_NONE && (BestIdx == INDEX_NONE || Idx < BestIdx))
                        {
                            BestIdx = Idx;
                            BestLen = Stop.Len();
                        }
                    }
                    if (BestIdx != INDEX_NONE)
                    {
                        FullOut = FullOut.Left(BestIdx);
                        break;
                    }
                }
            }

            // Decode sampled token
            llama_batch NextBatch = Api->llama_batch_get_one(&Tok, 1);
            const int32_t DecodeRes = Api->llama_decode(Ctx, NextBatch);
            if (DecodeRes != 0)
            {
                Api->llama_sampler_free(Sampler);
                Api->llama_free(Ctx);
                DispatchError(FString::Printf(TEXT("llama_decode(token) failed: %d"), (int32)DecodeRes));
                return;
            }
        }
        GenDone = FPlatformTime::Seconds();

        Api->llama_sampler_free(Sampler);
        Api->llama_free(Ctx);

        if (CVarLocalTalkerLogLLMPerf.GetValueOnAnyThread() != 0)
        {
            const double Total = GenDone - Start;
            const double AcquireSec = (AcquireDone > 0.0) ? (AcquireDone - Start) : 0.0;
            const double TokenizeSec = (TokenizeDone > 0.0 && AcquireDone > 0.0) ? (TokenizeDone - AcquireDone) : 0.0;
            const double PromptEvalSec = (PromptEvalDone > 0.0 && TokenizeDone > 0.0) ? (PromptEvalDone - TokenizeDone) : 0.0;
            const double GenSec = (GenDone > 0.0 && GenStart > 0.0) ? (GenDone - GenStart) : 0.0;
            const double TPS = (GenSec > 0.0) ? ((double)GeneratedTokens / GenSec) : 0.0;
            UE_LOG(
                LogLocalTalker,
                Log,
                TEXT("[LLMPerf] promptChars=%d promptTokens=%d used=%d trimmed=%d ctx=%d genTokens=%d acquire=%.3fs tokenize=%.3fs promptEval=%.3fs gen=%.3fs total=%.3fs tps=%.1f"),
                Prompt.Len(),
                PromptTokenCount,
                PromptTokenCountUsed,
                PromptTokensTrimmed,
                UsedContextTokens,
                GeneratedTokens,
                AcquireSec,
                TokenizeSec,
                PromptEvalSec,
                GenSec,
                Total,
                TPS
            );
        }

        if (bCancel)
        {
            if (CVarLocalTalkerTraceConversation_InProc.GetValueOnAnyThread() != 0)
            {
                const double End = FPlatformTime::Seconds();
                UE_LOG(LogLocalTalker, Log, TEXT("[TalkTrace][LLM] Completed (cancelled) outLen=%d duration=%.3fs"), FullOut.Len(), (End - Start));
            }
            DispatchCompleted(this, FullOut);
            return;
        }

        if (CVarLocalTalkerTraceConversation_InProc.GetValueOnAnyThread() != 0)
        {
            const double End = FPlatformTime::Seconds();
            UE_LOG(
                LogLocalTalker,
                Log,
                TEXT("[TalkTrace][LLM] Completed outLen=%d duration=%.3fs activateToStart=%.3fs"),
                FullOut.Len(),
                (End - Start),
                (Start - ActivateStart)
            );
        }
        DispatchCompleted(this, FullOut);
    });
}
