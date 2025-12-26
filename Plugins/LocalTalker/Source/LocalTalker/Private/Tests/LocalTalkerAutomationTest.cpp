#include "Misc/AutomationTest.h"
#include "LocalTalkerInProcAsync.h"
#include "LocalTalkerSettings.h"
#include "LocalTalkerTypes.h"
#include "LocalTalkConversationSubsystem.h"
#include "LocalTalkerLlamaCache.h"
#include "LocalLlamaDyn.h"
#include "HAL/PlatformFilemanager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/Actor.h"
#include "LocalCharacterComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

// Helper to resolve default paths
namespace LocalTalkerTestHelpers
{
    FString GetPluginBaseDir()
    {
        if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LocalTalker")))
        {
            return Plugin->GetBaseDir();
        }
        return FString();
    }

    FLocalTalkerRuntimePaths GetResolvedPaths()
    {
        const ULocalTalkerSettings* Settings = GetDefault<ULocalTalkerSettings>();
        FLocalTalkerRuntimePaths Paths = Settings ? Settings->DefaultPaths : FLocalTalkerRuntimePaths();
        
        const FString BaseDir = GetPluginBaseDir();
        if (!BaseDir.IsEmpty())
        {
            if (Paths.LlamaLibPath.IsEmpty())
            {
                Paths.LlamaLibPath = FPaths::Combine(BaseDir, TEXT("ThirdParty/llama/Win64/Release/libllama.dll"));
            }
            if (Paths.LlamaModelPath.IsEmpty())
            {
                Paths.LlamaModelPath = FPaths::Combine(BaseDir, TEXT("Resources/Models/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"));
            }
            if (Paths.PiperExePath.IsEmpty())
            {
                Paths.PiperExePath = FPaths::Combine(BaseDir, TEXT("ThirdParty/piper/Win64/Release/piper.exe"));
            }
            if (Paths.PiperVoiceModelPath.IsEmpty())
            {
                Paths.PiperVoiceModelPath = FPaths::Combine(BaseDir, TEXT("Resources/Voices/en_US-lessac-small.onnx"));
            }
        }
        return Paths;
    }

    // Check for required ggml dependency DLLs
    TArray<FString> GetMissingDependencyDlls(const FString& LlamaLibPath)
    {
        TArray<FString> Missing;
        const FString DllDir = FPaths::GetPath(FPaths::ConvertRelativePathToFull(LlamaLibPath));
        
        // Required core DLLs
        const TCHAR* RequiredDlls[] = {
            TEXT("ggml.dll"),
            TEXT("ggml-base.dll"),
            TEXT("ggml-cpu.dll")
        };
        
        for (const TCHAR* DllName : RequiredDlls)
        {
            const FString DllPath = FPaths::Combine(DllDir, DllName);
            if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*DllPath))
            {
                Missing.Add(DllName);
            }
        }
        
        return Missing;
    }

    // Check for GPU backend DLLs
    bool HasVulkanBackend(const FString& LlamaLibPath)
    {
        const FString DllDir = FPaths::GetPath(FPaths::ConvertRelativePathToFull(LlamaLibPath));
        return FPlatformFileManager::Get().GetPlatformFile().FileExists(*FPaths::Combine(DllDir, TEXT("ggml-vulkan.dll")));
    }

    bool HasCudaBackend(const FString& LlamaLibPath)
    {
        const FString DllDir = FPaths::GetPath(FPaths::ConvertRelativePathToFull(LlamaLibPath));
        return FPlatformFileManager::Get().GetPlatformFile().FileExists(*FPaths::Combine(DllDir, TEXT("ggml-cuda.dll")));
    }
}

// =============================================================================
// TEST: Verify all required runtime files exist
// This is the critical test that catches missing dependencies BEFORE runtime
// =============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLocalTalkerRequiredFilesTest,
    "LocalTalker.Files.RequiredExist",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FLocalTalkerRequiredFilesTest::RunTest(const FString& Parameters)
{
    using namespace LocalTalkerTestHelpers;
    
    const FLocalTalkerRuntimePaths Paths = GetResolvedPaths();
    bool bAllFilesExist = true;

    // Check libllama.dll
    {
        const bool bExists = FPlatformFileManager::Get().GetPlatformFile().FileExists(*Paths.LlamaLibPath);
        TestTrue(TEXT("libllama.dll exists"), bExists);
        if (!bExists)
        {
            AddError(FString::Printf(TEXT("MISSING: libllama.dll at %s"), *Paths.LlamaLibPath));
            bAllFilesExist = false;
        }
    }

    // Check GGUF model file - THIS IS CRITICAL
    {
        const bool bExists = FPlatformFileManager::Get().GetPlatformFile().FileExists(*Paths.LlamaModelPath);
        TestTrue(TEXT("LLM model (.gguf) exists"), bExists);
        if (!bExists)
        {
            AddError(FString::Printf(TEXT("MISSING: LLM model at %s. Download from: https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"), *Paths.LlamaModelPath));
            bAllFilesExist = false;
        }
    }

    // Check piper.exe
    {
        const bool bExists = FPlatformFileManager::Get().GetPlatformFile().FileExists(*Paths.PiperExePath);
        TestTrue(TEXT("piper.exe exists"), bExists);
        if (!bExists)
        {
            AddError(FString::Printf(TEXT("MISSING: piper.exe at %s"), *Paths.PiperExePath));
            bAllFilesExist = false;
        }
    }

    // Check voice model
    {
        const bool bExists = FPlatformFileManager::Get().GetPlatformFile().FileExists(*Paths.PiperVoiceModelPath);
        TestTrue(TEXT("Voice model (.onnx) exists"), bExists);
        if (!bExists)
        {
            AddError(FString::Printf(TEXT("MISSING: Voice model at %s"), *Paths.PiperVoiceModelPath));
            bAllFilesExist = false;
        }
    }

    // Check voice model JSON config (usually required by Piper)
    {
        const FString VoiceJsonPath = Paths.PiperVoiceModelPath + TEXT(".json");
        const bool bExists = FPlatformFileManager::Get().GetPlatformFile().FileExists(*VoiceJsonPath);
        TestTrue(TEXT("Voice model config (.onnx.json) exists"), bExists);
        if (!bExists)
        {
            AddError(FString::Printf(TEXT("MISSING: Voice model config at %s"), *VoiceJsonPath));
            bAllFilesExist = false;
        }
    }

    return bAllFilesExist;
}

// =============================================================================
// TEST: Verify llama.cpp dependency DLLs exist
// =============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLocalTalkerDependencyDllsTest,
    "LocalTalker.Files.DependencyDlls",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FLocalTalkerDependencyDllsTest::RunTest(const FString& Parameters)
{
    using namespace LocalTalkerTestHelpers;
    
    const FLocalTalkerRuntimePaths Paths = GetResolvedPaths();
    const FString DllDir = FPaths::GetPath(FPaths::ConvertRelativePathToFull(Paths.LlamaLibPath));
    
    AddInfo(FString::Printf(TEXT("Checking DLL directory: %s"), *DllDir));
    
    TArray<FString> MissingDlls = GetMissingDependencyDlls(Paths.LlamaLibPath);
    
    if (MissingDlls.Num() > 0)
    {
        for (const FString& Dll : MissingDlls)
        {
            AddError(FString::Printf(TEXT("MISSING dependency DLL: %s"), *Dll));
        }
        return false;
    }
    
    // Report GPU backend availability
    const bool bHasVulkan = HasVulkanBackend(Paths.LlamaLibPath);
    const bool bHasCuda = HasCudaBackend(Paths.LlamaLibPath);
    
    AddInfo(FString::Printf(TEXT("GPU Backends: Vulkan=%s, CUDA=%s"), 
        bHasVulkan ? TEXT("available") : TEXT("not found"),
        bHasCuda ? TEXT("available") : TEXT("not found")));
    
    if (!bHasVulkan && !bHasCuda)
    {
        AddWarning(TEXT("No GPU backend DLLs found (ggml-vulkan.dll or ggml-cuda.dll). LLM will run on CPU only."));
    }
    
    return true;
}

// =============================================================================
// TEST: Model file validation (size, format sanity check)
// =============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLocalTalkerModelValidationTest,
    "LocalTalker.Files.ModelValidation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FLocalTalkerModelValidationTest::RunTest(const FString& Parameters)
{
    using namespace LocalTalkerTestHelpers;
    
    const FLocalTalkerRuntimePaths Paths = GetResolvedPaths();

    // Model file must exist
    if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*Paths.LlamaModelPath))
    {
        AddError(FString::Printf(TEXT("Model file not found: %s"), *Paths.LlamaModelPath));
        return false;
    }

    // Check model file size
    // TinyLlama Q4_K_M is ~637 MB, so we require at least 100 MB for any valid LLM model
    // Files under this threshold are likely corrupt, incomplete downloads, or HTML error pages
    const int64 FileSize = FPlatformFileManager::Get().GetPlatformFile().FileSize(*Paths.LlamaModelPath);
    const int64 MinExpectedSize = 100 * 1024 * 1024; // 100 MB minimum for any real LLM model
    
    const double SizeMB = FileSize / (1024.0 * 1024.0);
    
    TestTrue(TEXT("Model file has reasonable size (> 100MB)"), FileSize > MinExpectedSize);
    if (FileSize <= MinExpectedSize)
    {
        AddError(FString::Printf(TEXT("Model file is too small (%.1f MB). Expected at least 100 MB. File may be corrupt or incomplete download: %s"), SizeMB, *Paths.LlamaModelPath));
        AddError(TEXT("Download the correct model from: https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_K_M.gguf"));
        return false;
    }

    // Check GGUF magic bytes (first 4 bytes should be "GGUF")
    TUniquePtr<IFileHandle> FileHandle(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Paths.LlamaModelPath));
    if (FileHandle)
    {
        uint8 Magic[4];
        if (FileHandle->Read(Magic, 4))
        {
            // GGUF magic: 0x46554747 ("GGUF" in little-endian)
            const bool bValidMagic = (Magic[0] == 'G' && Magic[1] == 'G' && Magic[2] == 'U' && Magic[3] == 'F');
            TestTrue(TEXT("Model file has valid GGUF magic header"), bValidMagic);
            if (!bValidMagic)
            {
                AddError(FString::Printf(TEXT("Invalid GGUF magic header. Got: 0x%02X%02X%02X%02X, expected: 'GGUF'"), Magic[0], Magic[1], Magic[2], Magic[3]));
                return false;
            }
        }
    }

    AddInfo(FString::Printf(TEXT("Model file validated: %s (%.1f MB, GGUF format confirmed)"), *Paths.LlamaModelPath, SizeMB));
    return true;
}

// =============================================================================
// TEST: DLL loads successfully with all function pointers resolved
// This simulates EXACTLY what UE5 does at runtime
// =============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLocalTalkerDllLoadIntegrationTest,
    "LocalTalker.Integration.DllLoad",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FLocalTalkerDllLoadIntegrationTest::RunTest(const FString& Parameters)
{
    using namespace LocalTalkerTestHelpers;
    
    const FLocalTalkerRuntimePaths Paths = GetResolvedPaths();
    
    // Verify DLL exists first
    if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*Paths.LlamaLibPath))
    {
        AddError(FString::Printf(TEXT("libllama.dll not found: %s"), *Paths.LlamaLibPath));
        return false;
    }
    
    // Create a test API instance and attempt to load
    // This tests the EXACT same code path that runtime uses
    FLocalLlamaApi TestApi;
    FString LoadError;
    
    const bool bLoaded = TestApi.Load(Paths.LlamaLibPath, LoadError);
    
    TestTrue(TEXT("libllama.dll loads successfully"), bLoaded);
    
    if (!bLoaded)
    {
        AddError(FString::Printf(TEXT("Failed to load libllama.dll: %s"), *LoadError));
        
        // Provide additional diagnostics
        TArray<FString> MissingDeps = GetMissingDependencyDlls(Paths.LlamaLibPath);
        if (MissingDeps.Num() > 0)
        {
            AddError(TEXT("Missing dependency DLLs that may be causing this:"));
            for (const FString& Dep : MissingDeps)
            {
                AddError(FString::Printf(TEXT("  - %s"), *Dep));
            }
        }
        
        return false;
    }
    
    // Verify critical function pointers are resolved
    TestNotNull(TEXT("llama_backend_init resolved"), (void*)TestApi.llama_backend_init);
    TestNotNull(TEXT("llama_model_default_params resolved"), (void*)TestApi.llama_model_default_params);
    TestNotNull(TEXT("llama_model_load_from_file resolved"), (void*)TestApi.llama_model_load_from_file);
    TestNotNull(TEXT("llama_model_free resolved"), (void*)TestApi.llama_model_free);
    TestNotNull(TEXT("llama_init_from_model resolved"), (void*)TestApi.llama_init_from_model);
    TestNotNull(TEXT("llama_tokenize resolved"), (void*)TestApi.llama_tokenize);
    TestNotNull(TEXT("llama_decode resolved"), (void*)TestApi.llama_decode);
    
    // Check optional GPU-related functions
    if (TestApi.llama_supports_gpu_offload)
    {
        AddInfo(TEXT("llama_supports_gpu_offload function is available"));
    }
    else
    {
        AddWarning(TEXT("llama_supports_gpu_offload function not found - GPU detection may be limited"));
    }
    
    if (TestApi.llama_print_system_info)
    {
        AddInfo(TEXT("llama_print_system_info function is available"));
    }
    
    // Clean up - unload the test DLL
    TestApi.Unload();
    
    AddInfo(TEXT("DLL load integration test passed - all required symbols resolved"));
    return true;
}

// =============================================================================
// TEST: Full model load integration test
// This is the CRITICAL test that simulates exactly what happens at game runtime
// =============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLocalTalkerModelLoadIntegrationTest,
    "LocalTalker.Integration.ModelLoad",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FLocalTalkerModelLoadIntegrationTest::RunTest(const FString& Parameters)
{
    using namespace LocalTalkerTestHelpers;
    
    const FLocalTalkerRuntimePaths Paths = GetResolvedPaths();
    
    // First, verify the model file is accessible
    {
        TUniquePtr<IFileHandle> TestHandle(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Paths.LlamaModelPath));
        if (!TestHandle)
        {
            AddError(FString::Printf(TEXT("Cannot open model file for reading: %s"), *Paths.LlamaModelPath));
            return false;
        }
        TestHandle.Reset();
    }
    
    // Use the actual LlamaCache to test the exact runtime code path
    FLocalLlamaApi* OutApi = nullptr;
    llama_model* OutModel = nullptr;
    const llama_vocab* OutVocab = nullptr;
    FString OutError;
    
    // This calls the EXACT same code that runs during gameplay
    const bool bSuccess = FLocalTalkerLlamaCache::Get().Acquire(
        Paths.LlamaLibPath,
        Paths.LlamaModelPath,
        0,  // GpuLayers = 0 means auto-detect
        ELocalTalkerGpuBackend::Auto,
        OutApi,
        OutModel,
        OutVocab,
        OutError
    );
    
    TestTrue(TEXT("Model loaded successfully via LlamaCache"), bSuccess);
    
    if (!bSuccess)
    {
        AddError(FString::Printf(TEXT("Model load failed: %s"), *OutError));
        
        // Provide detailed diagnostics
        AddError(TEXT("Diagnostic information:"));
        AddError(FString::Printf(TEXT("  DLL Path: %s"), *Paths.LlamaLibPath));
        AddError(FString::Printf(TEXT("  Model Path: %s"), *Paths.LlamaModelPath));
        
        // Check file sizes
        const int64 DllSize = FPlatformFileManager::Get().GetPlatformFile().FileSize(*Paths.LlamaLibPath);
        const int64 ModelSize = FPlatformFileManager::Get().GetPlatformFile().FileSize(*Paths.LlamaModelPath);
        AddError(FString::Printf(TEXT("  DLL Size: %.2f MB"), DllSize / (1024.0 * 1024.0)));
        AddError(FString::Printf(TEXT("  Model Size: %.2f MB"), ModelSize / (1024.0 * 1024.0)));
        
        // Check GPU backends
        AddError(FString::Printf(TEXT("  Vulkan DLL: %s"), HasVulkanBackend(Paths.LlamaLibPath) ? TEXT("present") : TEXT("missing")));
        AddError(FString::Printf(TEXT("  CUDA DLL: %s"), HasCudaBackend(Paths.LlamaLibPath) ? TEXT("present") : TEXT("missing")));
        
        return false;
    }
    
    // Verify outputs are valid
    TestNotNull(TEXT("API pointer is valid"), OutApi);
    TestNotNull(TEXT("Model pointer is valid"), OutModel);
    TestNotNull(TEXT("Vocab pointer is valid"), OutVocab);
    
    // Log GPU capabilities
    if (OutApi && OutApi->llama_supports_gpu_offload)
    {
        const bool bGpuSupported = OutApi->llama_supports_gpu_offload();
        AddInfo(FString::Printf(TEXT("GPU offload support: %s"), bGpuSupported ? TEXT("YES") : TEXT("NO")));
    }
    
    // Log system info if available
    if (OutApi && OutApi->llama_print_system_info)
    {
        const char* SysInfo = OutApi->llama_print_system_info();
        if (SysInfo && SysInfo[0] != '\0')
        {
            AddInfo(FString::Printf(TEXT("llama.cpp system info: %s"), UTF8_TO_TCHAR(SysInfo)));
        }
        else
        {
            AddWarning(TEXT("llama_print_system_info returned empty - this may indicate an initialization issue"));
        }
    }
    
    // Verify model params struct was initialized correctly
    if (OutApi && OutApi->llama_model_default_params)
    {
        llama_model_params TestParams = OutApi->llama_model_default_params();
        // Check that default params have reasonable values
        AddInfo(FString::Printf(TEXT("Default n_gpu_layers: %d"), TestParams.n_gpu_layers));
        AddInfo(FString::Printf(TEXT("Default use_mmap: %s"), TestParams.use_mmap ? TEXT("true") : TEXT("false")));
        AddInfo(FString::Printf(TEXT("Model params struct size: %llu bytes"), (uint64)sizeof(llama_model_params)));
    }
    
    AddInfo(TEXT("Model load integration test PASSED - model ready for inference"));
    return true;
}

// =============================================================================
// TEST: GPU Backend detection and configuration
// =============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLocalTalkerGpuBackendTest,
    "LocalTalker.Integration.GpuBackend",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FLocalTalkerGpuBackendTest::RunTest(const FString& Parameters)
{
    using namespace LocalTalkerTestHelpers;
    
    const FLocalTalkerRuntimePaths Paths = GetResolvedPaths();
    const FString DllDir = FPaths::GetPath(FPaths::ConvertRelativePathToFull(Paths.LlamaLibPath));
    
    // Check for GPU backend DLLs
    const bool bHasVulkan = HasVulkanBackend(Paths.LlamaLibPath);
    const bool bHasCuda = HasCudaBackend(Paths.LlamaLibPath);
    
    AddInfo(FString::Printf(TEXT("GPU Backend DLL Check:")));
    AddInfo(FString::Printf(TEXT("  ggml-vulkan.dll: %s"), bHasVulkan ? TEXT("FOUND") : TEXT("not found")));
    AddInfo(FString::Printf(TEXT("  ggml-cuda.dll: %s"), bHasCuda ? TEXT("FOUND") : TEXT("not found")));
    
    if (bHasVulkan)
    {
        // Verify Vulkan DLL size is reasonable (should be ~50+ MB)
        const FString VulkanDllPath = FPaths::Combine(DllDir, TEXT("ggml-vulkan.dll"));
        const int64 VulkanSize = FPlatformFileManager::Get().GetPlatformFile().FileSize(*VulkanDllPath);
        const double VulkanSizeMB = VulkanSize / (1024.0 * 1024.0);
        
        TestTrue(TEXT("Vulkan DLL has reasonable size (> 10MB)"), VulkanSize > 10 * 1024 * 1024);
        AddInfo(FString::Printf(TEXT("  Vulkan DLL size: %.1f MB"), VulkanSizeMB));
    }
    
    // Load API and check runtime GPU support
    FLocalLlamaApi TestApi;
    FString LoadError;
    
    if (TestApi.Load(Paths.LlamaLibPath, LoadError))
    {
        // Initialize backend to enable GPU detection
        if (TestApi.llama_backend_init)
        {
            TestApi.llama_backend_init();
        }
        
        // Check GPU support at runtime
        if (TestApi.llama_supports_gpu_offload)
        {
            const bool bGpuSupported = TestApi.llama_supports_gpu_offload();
            AddInfo(FString::Printf(TEXT("Runtime GPU offload: %s"), bGpuSupported ? TEXT("SUPPORTED") : TEXT("not available")));
            
            // If we have Vulkan DLL but GPU isn't supported, that's a potential issue
            if (bHasVulkan && !bGpuSupported)
            {
                AddWarning(TEXT("Vulkan DLL present but GPU offload reports unavailable. Check Vulkan drivers."));
            }
        }
        
        // Print system info
        if (TestApi.llama_print_system_info)
        {
            const char* SysInfo = TestApi.llama_print_system_info();
            if (SysInfo && SysInfo[0] != '\0')
            {
                AddInfo(FString::Printf(TEXT("System capabilities: %s"), UTF8_TO_TCHAR(SysInfo)));
            }
            else
            {
                AddWarning(TEXT("llama_print_system_info returned empty - backend may not be fully initialized"));
            }
        }
        
        // Cleanup
        if (TestApi.llama_backend_free)
        {
            TestApi.llama_backend_free();
        }
        TestApi.Unload();
    }
    else
    {
        AddError(FString::Printf(TEXT("Could not load DLL for GPU test: %s"), *LoadError));
        return false;
    }
    
    return true;
}

// =============================================================================
// TEST: Conversation subsystem registration
// =============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLocalTalkerConversationSubsystemTest,
    "LocalTalker.Subsystem.Registration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FLocalTalkerConversationSubsystemTest::RunTest(const FString& Parameters)
{
    // Get world from engine context
    UWorld* World = nullptr;
    if (GEngine && GEngine->GetWorldContexts().Num() > 0)
    {
        World = GEngine->GetWorldContexts()[0].World();
    }
    
    if (!World)
    {
        AddError(TEXT("No world context available"));
        return false;
    }

    ULocalTalkConversationSubsystem* Subsystem = World->GetSubsystem<ULocalTalkConversationSubsystem>();
    TestNotNull(TEXT("Conversation subsystem exists"), Subsystem);

    if (!Subsystem)
    {
        return false;
    }

    // Subsystem should be initialized
    TestTrue(TEXT("Subsystem is valid"), IsValid(Subsystem));

    return true;
}

// =============================================================================
// TEST: Backpressure prevents LLM->TTS from getting too far ahead
// =============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLocalTalkerPacingBackpressureTest,
    "LocalTalker.Pacing.Backpressure",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FLocalTalkerPacingBackpressureTest::RunTest(const FString& Parameters)
{
    UWorld* World = nullptr;
    if (GEngine && GEngine->GetWorldContexts().Num() > 0)
    {
        World = GEngine->GetWorldContexts()[0].World();
    }
    if (!World)
    {
        AddError(TEXT("No world context available"));
        return false;
    }

    AActor* A = World->SpawnActor<AActor>();
    TestNotNull(TEXT("Spawned actor"), A);
    if (!A) return false;

    ULocalCharacterComponent* C = NewObject<ULocalCharacterComponent>(A);
    TestNotNull(TEXT("Created LocalCharacterComponent"), C);
    if (!C) return false;
    C->RegisterComponent();

    // Make sentence extraction very easy
    C->bSpeakStreaming = true;
    C->MinCharsBeforeSpeak = 1;
    C->MaxSentenceChars = 200;
    C->FlushSeconds = 0.0f;

    // Case 1: Not backpressured -> should enqueue a sentence (increments pending sentence count)
    C->Test_SetPendingCounts(0, 0);
    C->MaxQueuedSentencesAhead = 2;
    C->MaxQueuedAudioChunksAhead = 3;
    C->Test_SetLLMTextBuffer(TEXT("Hello there."));
    C->TickComponent(0.016f, LEVELTICK_All, nullptr);

    TestTrue(TEXT("PendingSentenceCount increased when not backpressured"), C->Test_GetPendingSentenceCount() > 0);

    // Case 2: Backpressured -> should NOT enqueue more sentences, and should cap LLMTextBuffer
    C->Test_SetPendingCounts(5, 5);
    C->MaxQueuedSentencesAhead = 2;
    C->MaxQueuedAudioChunksAhead = 3;
    C->MaxBufferedCharsWhileBackpressured = 256;

    FString Big;
    Big.Reserve(2000);
    for (int32 i = 0; i < 2000; i++) { Big += TEXT("a"); }
    Big += TEXT(".");
    C->Test_SetLLMTextBuffer(Big);

    const int32 BeforePending = C->Test_GetPendingSentenceCount();
    C->TickComponent(0.016f, LEVELTICK_All, nullptr);

    TestEqual(TEXT("PendingSentenceCount unchanged while backpressured"), C->Test_GetPendingSentenceCount(), BeforePending);
    TestTrue(TEXT("LLM buffer capped while backpressured"), C->Test_GetLLMTextBufferLen() <= 256);

    return true;
}

// =============================================================================
// TEST: Audio chunk sample-rate is applied before QueueAudio (prevents 'fast speech')
// =============================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLocalTalkerPacingSampleRateTest,
    "LocalTalker.Pacing.AudioChunkSampleRate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter
)

bool FLocalTalkerPacingSampleRateTest::RunTest(const FString& Parameters)
{
    UWorld* World = nullptr;
    if (GEngine && GEngine->GetWorldContexts().Num() > 0)
    {
        World = GEngine->GetWorldContexts()[0].World();
    }
    if (!World)
    {
        AddError(TEXT("No world context available"));
        return false;
    }

    AActor* A = World->SpawnActor<AActor>();
    TestNotNull(TEXT("Spawned actor"), A);
    if (!A) return false;

    ULocalCharacterComponent* C = NewObject<ULocalCharacterComponent>(A);
    TestNotNull(TEXT("Created LocalCharacterComponent"), C);
    if (!C) return false;
    C->RegisterComponent();
    C->Test_InitAudio();

    // Enqueue a chunk tagged as 16kHz mono and pump it.
    C->Test_SetPendingCounts(0, 0);
    C->Test_EnqueueAudioChunk(/*SampleRate*/16000, /*NumChannels*/1, /*NumSamples*/16000);
    C->Test_PumpAudio();

    int32 SR = 0, CH = 0;
    C->Test_GetProcFormat(SR, CH);

    TestEqual(TEXT("Procedural wave sample rate matches chunk sample rate"), SR, 16000);
    TestEqual(TEXT("Procedural wave channel count matches chunk channels"), CH, 1);

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
