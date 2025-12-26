#include "LocalTalkerInProcAsync.h"
#include "LocalLlamaDyn.h"
#include "LocalTalkerLlamaCache.h"

#include "Async/Async.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"

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
    FString P;

    const FString Directions = !C.Directions.IsEmpty() ? C.Directions : C.SystemPrompt;
    const FString Desc = !C.CharacterDescription.IsEmpty() ? C.CharacterDescription : C.Persona;

    if (!Directions.IsEmpty())
    {
        P += TEXT("Directions:\n") + Directions + TEXT("\n\n");
    }
    if (!Desc.IsEmpty())
    {
        P += TEXT("Character Description:\n") + Desc + TEXT("\n\n");
    }

    P += TEXT("User: ") + UserPrompt + TEXT("\nAssistant:");
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

    Async(EAsyncExecution::ThreadPool, [this, P, C, Prompt]()
    {
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
            char Tmp[256];
            int32_t Written = Api->llama_detokenize(Vocab, &Tok, 1, Tmp, (int32_t)sizeof(Tmp), true, false);
            FString Piece;
            if (Written >= 0)
            {
                if (Written < (int32_t)sizeof(Tmp))
                {
                    Tmp[Written] = '\0';
                    Piece = UTF8_TO_TCHAR(Tmp);
                }
                else
                {
                    // Extremely rare, but avoid out-of-bounds.
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

            if (!Piece.IsEmpty())
            {
                FullOut += Piece;
                DispatchToken(this, Piece);
                DispatchDelta(this, Piece);

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
            DispatchCompleted(this, FullOut);
            return;
        }

        DispatchCompleted(this, FullOut);
    });
}
