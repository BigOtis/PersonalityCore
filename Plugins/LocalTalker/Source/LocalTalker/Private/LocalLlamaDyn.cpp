#include "LocalLlamaDyn.h"
#include "LocalTalkerLog.h"
#include "HAL/PlatformProcess.h"
#include "Misc/PathViews.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformMisc.h"

static FARPROC GetSym(HMODULE Lib, const char* Name) { return ::GetProcAddress(Lib, Name); }

template <typename T>
static bool LoadFn(HMODULE Lib, const char* Name, T& OutFn, FString& OutErr)
{
    FARPROC P = GetSym(Lib, Name);
    if (!P)
    {
        OutErr = FString::Printf(TEXT("Missing symbol in libllama: %s"), UTF8_TO_TCHAR(Name));
        return false;
    }
    OutFn = reinterpret_cast<T>(P);
    return true;
}

template <typename T>
static bool LoadOptionalFn(HMODULE Lib, const char* Name, T& OutFn)
{
    FARPROC P = GetSym(Lib, Name);
    if (!P)
    {
        OutFn = nullptr;
        return false;
    }
    OutFn = reinterpret_cast<T>(P);
    return true;
}

bool FLocalLlamaApi::Load(const FString& DllPath, FString& OutErr)
{
#if !PLATFORM_WINDOWS
    OutErr = TEXT("libllama dynamic loading is only implemented for Win64 in this starter.");
    return false;
#else
    if (bLoaded) return true;

    FString Abs = FPaths::ConvertRelativePathToFull(DllPath);
    FPaths::NormalizeFilename(Abs);
    FPaths::MakePlatformFilename(Abs); // ensure backslashes on Windows

    if (!FPaths::FileExists(Abs))
    {
        OutErr = FString::Printf(TEXT("Failed to load libllama.dll (file not found): %s"), *Abs);
        return false;
    }

    const FString DllDir = FPaths::GetPath(Abs);

    // UE may run with hardened DLL search paths; push the DLL directory so dependencies (ggml*.dll) can be found reliably.
    FPlatformProcess::PushDllDirectory(*DllDir);
    ON_SCOPE_EXIT
    {
        FPlatformProcess::PopDllDirectory(*DllDir);
    };

    // Preload common ggml dependency DLLs when present (helps in hardened search mode).
    // Load core DLLs first (required for CPU backend)
    const TCHAR* CoreDeps[] = { TEXT("ggml.dll"), TEXT("ggml-base.dll"), TEXT("ggml-cpu.dll") };
    HMODULE GgmlLib = nullptr;

    for (const TCHAR* DepName : CoreDeps)
    {
        const FString DepPath = FPaths::Combine(DllDir, DepName);
        if (FPaths::FileExists(DepPath))
        {
            HMODULE H = ::LoadLibraryExW(*DepPath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (H && FCString::Strcmp(DepName, TEXT("ggml.dll")) == 0)
            {
                GgmlLib = H;
            }
            if (!H)
            {
                const uint32 Err = (uint32)::GetLastError();
                UE_LOG(LogLocalTalker, Warning, TEXT("Failed to preload %s (error %u)."), DepName, Err);
            }
        }
    }
    
    // Use altered search path so dependent DLLs are resolved relative to the DLL directory.
    Lib = ::LoadLibraryExW(*Abs, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!Lib)
    {
        const uint32 Err = (uint32)::GetLastError();
        TCHAR SysMsg[1024];
        FPlatformMisc::GetSystemErrorMessage(SysMsg, UE_ARRAY_COUNT(SysMsg), Err);

        OutErr = FString::Printf(
            TEXT("Failed to load libllama.dll: %s (Win32=%u: %s)\n")
            TEXT("Common causes:\n")
            TEXT("- Missing dependency DLLs next to libllama (ggml*.dll)\n")
            TEXT("- Missing VC++ runtime\n")
            TEXT("- libllama built for wrong architecture (must be Win64)\n"),
            *Abs,
            Err,
            SysMsg
        );
        return false;
    }

    bool Ok = true;
    Ok &= LoadFn(Lib, "llama_backend_init", llama_backend_init, OutErr);
    Ok &= LoadFn(Lib, "llama_backend_free", llama_backend_free, OutErr);

    Ok &= LoadFn(Lib, "llama_model_default_params", llama_model_default_params, OutErr);
    Ok &= LoadFn(Lib, "llama_context_default_params", llama_context_default_params, OutErr);

    Ok &= LoadFn(Lib, "llama_model_load_from_file", llama_model_load_from_file, OutErr);
    Ok &= LoadFn(Lib, "llama_model_free", llama_model_free, OutErr);

    Ok &= LoadFn(Lib, "llama_init_from_model", llama_init_from_model, OutErr);
    // Deprecated fallback (not required)
    LoadOptionalFn(Lib, "llama_new_context_with_model", llama_new_context_with_model);
    Ok &= LoadFn(Lib, "llama_free", llama_free, OutErr);

    Ok &= LoadFn(Lib, "llama_get_model", llama_get_model, OutErr);
    Ok &= LoadFn(Lib, "llama_model_get_vocab", llama_model_get_vocab, OutErr);

    Ok &= LoadFn(Lib, "llama_tokenize", llama_tokenize, OutErr);
    Ok &= LoadFn(Lib, "llama_detokenize", llama_detokenize, OutErr);
    // Optional newer API - preferred for correct piece decoding (spaces) per token.
    LoadOptionalFn(Lib, "llama_token_to_piece", llama_token_to_piece);

    Ok &= LoadFn(Lib, "llama_batch_init", llama_batch_init, OutErr);
    Ok &= LoadFn(Lib, "llama_batch_free", llama_batch_free, OutErr);
    Ok &= LoadFn(Lib, "llama_batch_get_one", llama_batch_get_one, OutErr);

    Ok &= LoadFn(Lib, "llama_decode", llama_decode, OutErr);
    Ok &= LoadFn(Lib, "llama_get_logits_ith", llama_get_logits_ith, OutErr);

    // Optional (older builds may not export it; context params already include thread counts)
    LoadOptionalFn(Lib, "llama_set_n_threads", llama_set_n_threads);

    // Optional GPU/system info helpers (used for diagnostics + tests)
    LoadOptionalFn(Lib, "llama_supports_gpu_offload", llama_supports_gpu_offload);
    LoadOptionalFn(Lib, "llama_print_system_info", llama_print_system_info);

    // Load backend registration functions from ggml.dll if it was found
    if (GgmlLib)
    {
        LoadOptionalFn(GgmlLib, "ggml_backend_load_all", ggml_backend_load_all);
        LoadOptionalFn(GgmlLib, "ggml_backend_load_all_from_path", ggml_backend_load_all_from_path);
    }

    Ok &= LoadFn(Lib, "llama_vocab_is_eog", llama_vocab_is_eog, OutErr);

    Ok &= LoadFn(Lib, "llama_sampler_chain_default_params", llama_sampler_chain_default_params, OutErr);
    Ok &= LoadFn(Lib, "llama_sampler_chain_init", llama_sampler_chain_init, OutErr);
    Ok &= LoadFn(Lib, "llama_sampler_free", llama_sampler_free, OutErr);
    Ok &= LoadFn(Lib, "llama_sampler_init_temp", llama_sampler_init_temp, OutErr);
    Ok &= LoadFn(Lib, "llama_sampler_init_top_k", llama_sampler_init_top_k, OutErr);
    Ok &= LoadFn(Lib, "llama_sampler_init_top_p", llama_sampler_init_top_p, OutErr);
    Ok &= LoadFn(Lib, "llama_sampler_init_dist", llama_sampler_init_dist, OutErr);
    Ok &= LoadFn(Lib, "llama_sampler_init_greedy", llama_sampler_init_greedy, OutErr);
    Ok &= LoadFn(Lib, "llama_sampler_chain_add", llama_sampler_chain_add, OutErr);
    Ok &= LoadFn(Lib, "llama_sampler_sample", llama_sampler_sample, OutErr);
    Ok &= LoadFn(Lib, "llama_sampler_accept", llama_sampler_accept, OutErr);
    Ok &= LoadFn(Lib, "llama_sampler_reset", llama_sampler_reset, OutErr);

    // Optional: chat template helper (not used everywhere yet, but useful for future prompt formatting).
    LoadOptionalFn(Lib, "llama_chat_apply_template", llama_chat_apply_template);

    if (!Ok)
    {
        Unload();
        return false;
    }

    bLoaded = true;
    return true;
#endif
}

void FLocalLlamaApi::Unload()
{
#if PLATFORM_WINDOWS
    if (Lib)
    {
        ::FreeLibrary(Lib);
        Lib = nullptr;
    }
#endif
    bLoaded = false;
}
