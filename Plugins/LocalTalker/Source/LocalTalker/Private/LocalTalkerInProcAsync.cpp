#include "LocalTalkerInProcAsync.h"
#include "LocalLlamaDyn.h"
#include "LocalTalkerLlamaCache.h"
#include "LocalTalkerLog.h"

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

static bool LocalTalkerShouldStopEarly(const FString& FullOut)
{
    // Deterministic "dialogue mode" stopping rules:
    // - Stop at first newline
    // - Stop after a reasonable one-line length
    // - Stop after terminal punctuation once we have a minimal amount of content
    if (FullOut.Contains(TEXT("\n")) || FullOut.Contains(TEXT("\r")))
    {
        return true;
    }

    // Hard cap: keep it a single line suitable for speech.
    if (FullOut.Len() >= 220)
    {
        return true;
    }

    // If we already have a chunk, stop when we reach a sentence end.
    if (FullOut.Len() >= 40)
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

static FString BuildPrompt(const FLocalTalkerCharacterConfig& C, const FString& UserPrompt)
{
    // TinyLlama-1.1B-Chat-v1.0 follows a Zephyr-style chat template on HuggingFace:
    // <|system|>\n... </s>
    // <|user|>\n... </s>
    // <|assistant|>
    //
    // Matching the training template is the biggest lever to get stable formatting and spacing.

    const FString Directions = !C.Directions.IsEmpty() ? C.Directions : C.SystemPrompt;
    const FString Desc = !C.CharacterDescription.IsEmpty() ? C.CharacterDescription : C.Persona;

    FString SystemBlock;
    if (!Directions.IsEmpty())
    {
        SystemBlock += Directions.TrimStartAndEnd();
    }
    if (!Desc.IsEmpty())
    {
        if (!SystemBlock.IsEmpty()) SystemBlock += TEXT("\n\n");
        SystemBlock += Desc.TrimStartAndEnd();
    }

    if (!SystemBlock.IsEmpty()) SystemBlock += TEXT("\n\n");
    SystemBlock +=
        TEXT("RULES:\n")
        TEXT("- Reply as the character, in natural English.\n")
        TEXT("- Output only the spoken dialogue. Do not include speaker labels (no \"User:\", \"Assistant:\", or \"Name:\").\n")
        TEXT("- Do not output any markup or control tokens (no <|system|>, <|user|>, <|assistant|>, </s>, [INST], [/INST]).\n")
        TEXT("- Use normal spacing between words and standard punctuation.\n")
        TEXT("- Prefer a single paragraph; avoid lists unless explicitly asked.\n");

    // Use the tokenizer's expected role tags and EOS separator.
    // Note: llama.cpp uses </s> as the EOS token for Llama-family models.
    FString P;
    P.Reserve(SystemBlock.Len() + UserPrompt.Len() + 64);

    P += TEXT("<|system|>\n");
    P += SystemBlock;
    P += TEXT("</s>\n");

    P += TEXT("<|user|>\n");
    P += UserPrompt.TrimStartAndEnd();
    P += TEXT("</s>\n");

    // Generation prompt
    P += TEXT("<|assistant|>\n");
    return P;
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
    const FString Prompt = bPromptIsFull ? PromptText : BuildPrompt(C, UserPrompt);

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

    Async(EAsyncExecution::ThreadPool, [this, P, C, Prompt, ActivateStart]()
    {
        const double Start = FPlatformTime::Seconds();
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

        if (!Api || !Model || !Vocab)
        {
            DispatchError(TEXT("Internal error: llama shared state not initialized."));
            return;
        }

        // Context
        llama_context_params CParams = Api->llama_context_default_params();
        const int32 Cores = FMath::Max(1, FPlatformMisc::NumberOfCoresIncludingHyperthreads());
        CParams.n_threads = Cores;
        CParams.n_threads_batch = Cores;
        CParams.n_ctx = 2048;
        CParams.n_batch = 512;
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
                Api->llama_free(Ctx);
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
                Api->llama_free(Ctx);
                DispatchError(TEXT("llama_tokenize failed after resizing buffer."));
                return;
            }
        }

        PromptTokens.SetNum(NPrompt);

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

        // Sampler chain
        llama_sampler_chain_params SParams = Api->llama_sampler_chain_default_params();
        llama_sampler* Sampler = Api->llama_sampler_chain_init(SParams);
        if (!Sampler)
        {
            Api->llama_free(Ctx);
            DispatchError(TEXT("Failed to init llama sampler chain."));
            return;
        }

        // Basic fast sampler defaults; can be made configurable.
        Api->llama_sampler_chain_add(Sampler, Api->llama_sampler_init_top_k(40));
        Api->llama_sampler_chain_add(Sampler, Api->llama_sampler_init_top_p(0.95f, 1));

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

        for (int32 i = 0; i < C.MaxTokens; i++)
        {
            if (bCancel)
            {
                break;
            }

            llama_token Tok = Api->llama_sampler_sample(Sampler, Ctx, -1);
            Api->llama_sampler_accept(Sampler, Tok);

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

                if (LocalTalkerShouldStopEarly(FullOut))
                {
                    break;
                }

                // Check stop sequence only if configured (empty by default now)
                if (!C.Stop.IsEmpty() && FullOut.Contains(C.Stop))
                {
                    const int32 Cut = FullOut.Find(C.Stop);
                    if (Cut != INDEX_NONE)
                    {
                        FullOut = FullOut.Left(Cut);
                    }
                    break;
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

        Api->llama_sampler_free(Sampler);
        Api->llama_free(Ctx);

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
