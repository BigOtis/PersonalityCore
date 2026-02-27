using UnrealBuildTool;
using System.IO;

public class LocalTalker : ModuleRules
{
    public LocalTalker(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "Projects",
            "AudioMixer",
            "AudioCaptureCore",
            "DeveloperSettings",
            "Json",
            "JsonUtilities"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Slate",
            "SlateCore",
            "AutomationTest"
        });

        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.Add("UnrealEd");
        }


        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            string PluginDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
            string ThirdPartyDir = Path.Combine(PluginDir, "ThirdParty");
            string LlamaReleaseDir = Path.Combine(ThirdPartyDir, "llama", "Win64", "Release");

            // Headers (used for types even if we dynamically load the DLL)
            string LlamaIncludeDir = Path.Combine(ThirdPartyDir, "llama", "include");
            if (Directory.Exists(LlamaIncludeDir))
            {
                PublicIncludePaths.Add(LlamaIncludeDir);
            }

            // Runtime-staged artifacts (only add if present to avoid breaking dev builds)
            string LlamaDll = Path.Combine(ThirdPartyDir, "llama", "Win64", "Release", "libllama.dll");
            if (File.Exists(LlamaDll))
            {
                RuntimeDependencies.Add(LlamaDll);
            }

            // llama.cpp shared builds on Windows commonly depend on ggml DLLs in the same directory.
            string GgmlBase = Path.Combine(ThirdPartyDir, "llama", "Win64", "Release", "ggml-base.dll");
            string GgmlCpu  = Path.Combine(ThirdPartyDir, "llama", "Win64", "Release", "ggml-cpu.dll");
            string GgmlDll  = Path.Combine(ThirdPartyDir, "llama", "Win64", "Release", "ggml.dll");
            string GgmlVulkan = Path.Combine(ThirdPartyDir, "llama", "Win64", "Release", "ggml-vulkan.dll");
            string GgmlCuda   = Path.Combine(ThirdPartyDir, "llama", "Win64", "Release", "ggml-cuda.dll");
            if (File.Exists(GgmlBase)) RuntimeDependencies.Add(GgmlBase);
            if (File.Exists(GgmlCpu))  RuntimeDependencies.Add(GgmlCpu);
            if (File.Exists(GgmlDll))  RuntimeDependencies.Add(GgmlDll);
            if (File.Exists(GgmlVulkan)) RuntimeDependencies.Add(GgmlVulkan);
            if (File.Exists(GgmlCuda))   RuntimeDependencies.Add(GgmlCuda);

            // Newer llama.cpp Windows releases can ship additional runtime DLLs (e.g. libomp/libcurl, mtmd, multiple cpu backends).
            // Stage all DLLs in the release directory to maximize compatibility across machines.
            if (Directory.Exists(LlamaReleaseDir))
            {
                foreach (string Dll in Directory.GetFiles(LlamaReleaseDir, "*.dll"))
                {
                    RuntimeDependencies.Add(Dll);
                }
            }

            string LlamaLicense = Path.Combine(PluginDir, "Resources", "ThirdPartyNotices", "llama.cpp_LICENSE.txt");
            if (File.Exists(LlamaLicense))
            {
                RuntimeDependencies.Add(LlamaLicense);
            }

            // Default bundled artifacts (models/voices) - staged if present
            string DefaultModel = Path.Combine(PluginDir, "Resources", "Models", "Llama-3.2-3B-Instruct-Q6_K_L.gguf");
            if (File.Exists(DefaultModel))
            {
                RuntimeDependencies.Add(DefaultModel);
            }

            // Stage all bundled voice assets (including Qwen prompt assets).
            string VoicesDir = Path.Combine(PluginDir, "Resources", "Voices");
            if (Directory.Exists(VoicesDir))
            {
                RuntimeDependencies.Add(Path.Combine(VoicesDir, "**"));
            }

            // Stage runtime worker scripts/config files.
            string KokoroDir = Path.Combine(PluginDir, "Resources", "Kokoro");
            if (Directory.Exists(KokoroDir))
            {
                RuntimeDependencies.Add(Path.Combine(KokoroDir, "**"));
            }

            string WhisperDir = Path.Combine(PluginDir, "Resources", "Whisper");
            if (Directory.Exists(WhisperDir))
            {
                RuntimeDependencies.Add(Path.Combine(WhisperDir, "**"));
            }

        }
    }
}
