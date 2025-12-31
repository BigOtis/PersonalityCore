#include "LocalTalkerLlamaCache.h"
#include "LocalTalkerLog.h"
#include "LocalTalkerSettings.h"

#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

FLocalTalkerLlamaCache& FLocalTalkerLlamaCache::Get()
{
    static FLocalTalkerLlamaCache G;
    return G;
}

bool FLocalTalkerLlamaCache::Acquire(
    const FString& LlamaDllPath,
    const FString& ModelPath,
    int32 GpuLayers,
    ELocalTalkerGpuBackend Backend,
    FLocalLlamaApi*& OutApi,
    llama_model*& OutModel,
    const llama_vocab*& OutVocab,
    FString& OutError
)
{
    OutApi = nullptr;
    OutModel = nullptr;
    OutVocab = nullptr;
    OutError.Reset();

    FScopeLock Lock(&Mutex);

    // DLL (only one per runtime for safety)
    if (!Api.bLoaded)
    {
        if (!Api.Load(LlamaDllPath, OutError))
        {
            return false;
        }
        LoadedDllPath = LlamaDllPath;
        bBackendInit = false;
    }
    else if (!LoadedDllPath.Equals(LlamaDllPath, ESearchCase::IgnoreCase))
    {
        OutError = FString::Printf(
            TEXT("LocalTalker: changing LlamaLibPath at runtime is not supported (loaded: %s, requested: %s). Restart the editor/game."),
            *LoadedDllPath,
            *LlamaDllPath
        );
        return false;
    }

    // Initialize backend BEFORE using any llama functions
    if (!bBackendInit)
    {
        Api.llama_backend_init();
        
        // CRITICAL: Newer llama.cpp versions require explicit backend loading.
        // We tell it exactly where to find ggml-cpu.dll and ggml-vulkan.dll.
        if (Api.ggml_backend_load_all_from_path)
        {
            const FString DllDir = FPaths::GetPath(FPaths::ConvertRelativePathToFull(LlamaDllPath));
            FTCHARToUTF8 DllDirUtf8(*DllDir);
            Api.ggml_backend_load_all_from_path(DllDirUtf8.Get());
            UE_LOG(LogLocalTalker, Log, TEXT("Requested backend load from: %s"), *DllDir);
        }
        else if (Api.ggml_backend_load_all)
        {
            Api.ggml_backend_load_all();
        }
        
        bBackendInit = true;

        // Print system info after backend is initialized
        if (Api.llama_print_system_info)
        {
            const char* SysInfo = Api.llama_print_system_info();
            if (SysInfo && SysInfo[0] != '\0')
            {
                UE_LOG(LogLocalTalker, Log, TEXT("llama.cpp system info:\n%s"), UTF8_TO_TCHAR(SysInfo));
            }
            else
            {
                UE_LOG(LogLocalTalker, Warning, TEXT("llama_print_system_info returned empty - backends may not be loaded"));
            }
        }

        // Log GPU support status
        if (Api.llama_supports_gpu_offload)
        {
            const bool bGpuSupported = Api.llama_supports_gpu_offload();
            UE_LOG(LogLocalTalker, Log, TEXT("llama.cpp GPU offload: %s"), bGpuSupported ? TEXT("supported") : TEXT("not available"));
        }
    }

    // Resolve GPU layers and prefer Vulkan when requested/available.
    int32 ResolvedGpuLayers = GpuLayers;
    if (ResolvedGpuLayers < 0)
    {
        ResolvedGpuLayers = 0;
    }
    else if (ResolvedGpuLayers == 0)
    {
        // NOTE: llama_supports_gpu_offload() has been unreliable across some Windows builds/backends.
        // Vulkan/CUDA presence is primarily inferred by the backend DLL being present next to libllama.
        const bool bSupports = Api.llama_supports_gpu_offload ? Api.llama_supports_gpu_offload() : false;
        const FString DllDir = FPaths::GetPath(FPaths::ConvertRelativePathToFull(LlamaDllPath));
        const bool bHasVulkan = FPlatformFileManager::Get().GetPlatformFile().FileExists(*FPaths::Combine(DllDir, TEXT("ggml-vulkan.dll")));
        const bool bHasCuda   = FPlatformFileManager::Get().GetPlatformFile().FileExists(*FPaths::Combine(DllDir, TEXT("ggml-cuda.dll")));

        const bool bWantVulkan = (Backend == ELocalTalkerGpuBackend::Vulkan) || (Backend == ELocalTalkerGpuBackend::Auto);
        const bool bWantCuda   = (Backend == ELocalTalkerGpuBackend::CUDA);

        // Check if GPU is actually functional (not just DLL present)
        // This catches cases where vulkan-1.dll is missing or drivers aren't installed
        const bool bGpuFunctional = bSupports;
        
        const bool bCanVulkan = bHasVulkan && bGpuFunctional;
        const bool bCanCuda   = bHasCuda && bGpuFunctional;

        bool bEnable = false;
        if (Backend == ELocalTalkerGpuBackend::CPU)
        {
            bEnable = false;
        }
        else if (bWantVulkan && bCanVulkan)
        {
            bEnable = true;
        }
        else if (bWantCuda && bCanCuda)
        {
            bEnable = true;
        }
        else if (Backend == ELocalTalkerGpuBackend::Auto && bCanCuda)
        {
            // Auto prefers Vulkan, but CUDA is acceptable if Vulkan not present.
            bEnable = true;
        }

        if (Backend == ELocalTalkerGpuBackend::Vulkan && !bCanVulkan)
        {
            UE_LOG(LogLocalTalker, Warning, TEXT("Vulkan backend requested, but GPU offload is unavailable. supports_gpu_offload=%s, ggml-vulkan.dll=%s. Falling back to CPU."),
                bSupports ? TEXT("true") : TEXT("false"),
                bHasVulkan ? TEXT("present") : TEXT("missing"));
        }
        else if (Backend == ELocalTalkerGpuBackend::CUDA && !bCanCuda)
        {
            UE_LOG(LogLocalTalker, Warning, TEXT("CUDA backend requested, but GPU offload is unavailable. supports_gpu_offload=%s, ggml-cuda.dll=%s. Falling back to CPU."),
                bSupports ? TEXT("true") : TEXT("false"),
                bHasCuda ? TEXT("present") : TEXT("missing"));
        }
        else if (Backend == ELocalTalkerGpuBackend::Auto && !bHasVulkan && !bHasCuda)
        {
            UE_LOG(LogLocalTalker, Warning, TEXT("No GPU backend DLL was found next to libllama.dll (expected ggml-vulkan.dll and/or ggml-cuda.dll). Running CPU-only."));
        }
        else if (Backend == ELocalTalkerGpuBackend::Auto && (bHasVulkan || bHasCuda) && !bGpuFunctional)
        {
            UE_LOG(LogLocalTalker, Warning, TEXT("GPU backend DLL found but GPU offload is not functional (vulkan-1.dll may be missing or drivers not installed). Falling back to CPU."));
        }

        if (bEnable)
        {
            const ULocalTalkerSettings* S = GetDefault<ULocalTalkerSettings>();
            const int32 AutoCap = S ? S->AutoGpuLayerCap : 0;
            ResolvedGpuLayers = (AutoCap > 0) ? AutoCap : 999;
            if (AutoCap > 0)
            {
                UE_LOG(LogLocalTalker, Log, TEXT("Auto GPU layer cap applied: %d"), AutoCap);
            }
        }
        else
        {
            ResolvedGpuLayers = 0;
        }
    }

    const FString ModelKey = FString::Printf(TEXT("%s|gpu=%d|backend=%d"), *ModelPath, ResolvedGpuLayers, (int32)Backend);

    // Model cache (keyed by path + gpu layers)
    llama_model** ExistingModel = ModelsByKey.Find(ModelKey);
    if (!ExistingModel)
    {
        llama_model_params MParams = Api.llama_model_default_params();
        MParams.n_gpu_layers = ResolvedGpuLayers;
        MParams.use_mmap = true;
        MParams.use_mlock = false;

        FTCHARToUTF8 ModelPathUtf8(*ModelPath);
        llama_model* NewModel = Api.llama_model_load_from_file(ModelPathUtf8.Get(), MParams);
        if (!NewModel)
        {
            OutError = FString::Printf(TEXT("Failed to load model: %s"), *ModelPath);
            return false;
        }

        const llama_vocab* NewVocab = Api.llama_model_get_vocab(NewModel);
        if (!NewVocab)
        {
            Api.llama_model_free(NewModel);
            OutError = TEXT("llama_model_get_vocab returned null.");
            return false;
        }

        ModelsByKey.Add(ModelKey, NewModel);
        VocabsByKey.Add(ModelKey, NewVocab);
        ExistingModel = ModelsByKey.Find(ModelKey);

        UE_LOG(LogLocalTalker, Log, TEXT("Loaded llama model (gpu_layers=%d): %s"), ResolvedGpuLayers, *ModelPath);
    }

    const llama_vocab** ExistingVocab = VocabsByKey.Find(ModelKey);
    if (!ExistingModel || !ExistingVocab || !*ExistingModel || !*ExistingVocab)
    {
        OutError = TEXT("LocalTalker: internal model cache error.");
        return false;
    }

    OutApi = &Api;
    OutModel = *ExistingModel;
    OutVocab = *ExistingVocab;
    return true;
}

void FLocalTalkerLlamaCache::Shutdown()
{
    FScopeLock Lock(&Mutex);

    // Free all models
    for (TPair<FString, llama_model*>& KV : ModelsByKey)
    {
        if (KV.Value)
        {
            Api.llama_model_free(KV.Value);
            KV.Value = nullptr;
        }
    }
    ModelsByKey.Empty();
    VocabsByKey.Empty();

    if (Api.bLoaded && bBackendInit)
    {
        Api.llama_backend_free();
        bBackendInit = false;
    }

    Api.Unload();
    LoadedDllPath.Reset();
}
