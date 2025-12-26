#pragma once

#include "CoreMinimal.h"

// llama.h includes ggml headers and is expected to come from Plugins/LocalTalker/ThirdParty/llama/include
THIRD_PARTY_INCLUDES_START
#include "llama.h"
THIRD_PARTY_INCLUDES_END

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

struct FLocalLlamaApi
{
    bool bLoaded = false;

#if PLATFORM_WINDOWS
    HMODULE Lib = nullptr;
#endif

    using llama_backend_init_fn = void(*)(void);
    using llama_backend_free_fn = void(*)(void);

    using llama_model_default_params_fn = llama_model_params(*)(void);
    using llama_context_default_params_fn = llama_context_params(*)(void);

    using llama_model_load_from_file_fn = llama_model*(*)(const char*, llama_model_params);
    using llama_model_free_fn = void(*)(llama_model*);

    using llama_new_context_with_model_fn = llama_context*(*)(llama_model*, llama_context_params);
    using llama_free_fn = void(*)(llama_context*);

    using llama_init_from_model_fn = llama_context*(*)(llama_model*, llama_context_params);

    using llama_get_model_fn = const llama_model*(*)(const llama_context*);
    using llama_model_get_vocab_fn = const llama_vocab*(*)(const llama_model*);

    using llama_tokenize_fn = int32_t(*)(const llama_vocab*, const char*, int32_t, llama_token*, int32_t, bool, bool);
    using llama_detokenize_fn = int32_t(*)(const llama_vocab*, const llama_token*, int32_t, char*, int32_t, bool, bool);

    using llama_batch_init_fn = llama_batch(*)(int32_t, int32_t, int32_t);
    using llama_batch_free_fn = void(*)(llama_batch);
    using llama_batch_get_one_fn = llama_batch(*)(llama_token*, int32_t);

    using llama_decode_fn = int32_t(*)(llama_context*, llama_batch);

    using llama_get_logits_ith_fn = const float*(*)(llama_context*, int32_t);

    using llama_set_n_threads_fn = void(*)(llama_context*, int32_t, int32_t);

    using llama_supports_gpu_offload_fn = bool(*)(void);
    using llama_print_system_info_fn = const char*(*)(void);

    // GGML backend loading functions (loaded from ggml.dll)
    using ggml_backend_load_all_fn = void(*)(void);
    using ggml_backend_load_all_from_path_fn = void(*)(const char*);

    using llama_vocab_is_eog_fn = bool(*)(const llama_vocab*, llama_token);

    using llama_sampler_chain_default_params_fn = llama_sampler_chain_params(*)(void);
    using llama_sampler_chain_init_fn = llama_sampler*(*)(llama_sampler_chain_params);
    using llama_sampler_free_fn = void(*)(llama_sampler*);
    using llama_sampler_init_temp_fn = llama_sampler*(*)(float);
    using llama_sampler_init_top_k_fn = llama_sampler*(*)(int);
    using llama_sampler_init_top_p_fn = llama_sampler*(*)(float, int);
    using llama_sampler_init_dist_fn = llama_sampler*(*)(uint32_t);
    using llama_sampler_init_greedy_fn = llama_sampler*(*)(void);
    using llama_sampler_chain_add_fn = void(*)(llama_sampler*, llama_sampler*);
    using llama_sampler_sample_fn = llama_token(*)(llama_sampler*, llama_context*, int32_t);
    using llama_sampler_accept_fn = void(*)(llama_sampler*, llama_token);
    using llama_sampler_reset_fn = void(*)(llama_sampler*);

    llama_backend_init_fn llama_backend_init = nullptr;
    llama_backend_free_fn llama_backend_free = nullptr;

    llama_model_default_params_fn llama_model_default_params = nullptr;
    llama_context_default_params_fn llama_context_default_params = nullptr;

    llama_model_load_from_file_fn llama_model_load_from_file = nullptr;
    llama_model_free_fn llama_model_free = nullptr;

    llama_new_context_with_model_fn llama_new_context_with_model = nullptr;
    llama_free_fn llama_free = nullptr;
    llama_init_from_model_fn llama_init_from_model = nullptr;

    llama_get_model_fn llama_get_model = nullptr;
    llama_model_get_vocab_fn llama_model_get_vocab = nullptr;

    llama_tokenize_fn llama_tokenize = nullptr;
    llama_detokenize_fn llama_detokenize = nullptr;

    llama_batch_init_fn llama_batch_init = nullptr;
    llama_batch_free_fn llama_batch_free = nullptr;
    llama_batch_get_one_fn llama_batch_get_one = nullptr;

    llama_decode_fn llama_decode = nullptr;
    llama_get_logits_ith_fn llama_get_logits_ith = nullptr;

    llama_set_n_threads_fn llama_set_n_threads = nullptr;

    llama_supports_gpu_offload_fn llama_supports_gpu_offload = nullptr;
    llama_print_system_info_fn llama_print_system_info = nullptr;

    // GGML backend loading (required for newer llama.cpp versions)
    using ggml_backend_load_all_fn = void(*)(void);
    using ggml_backend_load_all_from_path_fn = void(*)(const char*);
    ggml_backend_load_all_fn ggml_backend_load_all = nullptr;
    ggml_backend_load_all_from_path_fn ggml_backend_load_all_from_path = nullptr;

    llama_vocab_is_eog_fn llama_vocab_is_eog = nullptr;

    llama_sampler_chain_default_params_fn llama_sampler_chain_default_params = nullptr;
    llama_sampler_chain_init_fn llama_sampler_chain_init = nullptr;
    llama_sampler_free_fn llama_sampler_free = nullptr;
    llama_sampler_init_temp_fn llama_sampler_init_temp = nullptr;
    llama_sampler_init_top_k_fn llama_sampler_init_top_k = nullptr;
    llama_sampler_init_top_p_fn llama_sampler_init_top_p = nullptr;
    llama_sampler_init_dist_fn llama_sampler_init_dist = nullptr;
    llama_sampler_init_greedy_fn llama_sampler_init_greedy = nullptr;
    llama_sampler_chain_add_fn llama_sampler_chain_add = nullptr;
    llama_sampler_sample_fn llama_sampler_sample = nullptr;
    llama_sampler_accept_fn llama_sampler_accept = nullptr;
    llama_sampler_reset_fn llama_sampler_reset = nullptr;

    bool Load(const FString& DllPath, FString& OutErr);
    void Unload();
};
