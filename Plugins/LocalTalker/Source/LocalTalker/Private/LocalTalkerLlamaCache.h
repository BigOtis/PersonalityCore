#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Containers/Map.h"

#include "LocalTalkerTypes.h"
#include "LocalLlamaDyn.h"

struct FLocalTalkerLlamaCache
{
public:
    static FLocalTalkerLlamaCache& Get();

    // Loads (once) the llama DLL and models. Never unloads during runtime; only on Shutdown().
    // This avoids invalidating pointers while contexts are running on other threads.
    bool Acquire(
        const FString& LlamaDllPath,
        const FString& ModelPath,
        int32 GpuLayers,
        ELocalTalkerGpuBackend Backend,
        FLocalLlamaApi*& OutApi,
        llama_model*& OutModel,
        const llama_vocab*& OutVocab,
        FString& OutError
    );

    void Shutdown();

private:
    FCriticalSection Mutex;

    bool bBackendInit = false;
    FString LoadedDllPath;

    FLocalLlamaApi Api;
    TMap<FString, llama_model*> ModelsByKey;
    TMap<FString, const llama_vocab*> VocabsByKey;
};


