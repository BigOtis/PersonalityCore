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

            // Piper (optional at dev time; bundled for Fab builds)
            string PiperExe = Path.Combine(ThirdPartyDir, "piper", "Win64", "Release", "piper.exe");
            if (File.Exists(PiperExe))
            {
                RuntimeDependencies.Add(PiperExe);
            }
            // Piper runtime DLLs + data folder (Windows release bundle)
            string PiperDir = Path.Combine(ThirdPartyDir, "piper", "Win64", "Release");
            string PiperEspeak = Path.Combine(PiperDir, "espeak-ng.dll");
            string PiperPhonemize = Path.Combine(PiperDir, "piper_phonemize.dll");
            string PiperOrt = Path.Combine(PiperDir, "onnxruntime.dll");
            string PiperOrtShared = Path.Combine(PiperDir, "onnxruntime_providers_shared.dll");
            string PiperTashkeel = Path.Combine(PiperDir, "libtashkeel_model.ort");

            if (File.Exists(PiperEspeak)) RuntimeDependencies.Add(PiperEspeak);
            if (File.Exists(PiperPhonemize)) RuntimeDependencies.Add(PiperPhonemize);
            if (File.Exists(PiperOrt)) RuntimeDependencies.Add(PiperOrt);
            if (File.Exists(PiperOrtShared)) RuntimeDependencies.Add(PiperOrtShared);
            if (File.Exists(PiperTashkeel)) RuntimeDependencies.Add(PiperTashkeel);

            // Stage the full espeak-ng-data directory if present (required for phonemization).
            string EspeakData = Path.Combine(PiperDir, "espeak-ng-data");
            if (Directory.Exists(EspeakData))
            {
                RuntimeDependencies.Add(Path.Combine(EspeakData, "**"));
            }

            // Default bundled artifacts (models/voices) - staged if present
            string DefaultModel = Path.Combine(PluginDir, "Resources", "Models", "Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf");
            if (File.Exists(DefaultModel))
            {
                RuntimeDependencies.Add(DefaultModel);
            }

            string DefaultVoice = Path.Combine(PluginDir, "Resources", "Voices", "en_US-lessac-small.onnx");
            if (File.Exists(DefaultVoice))
            {
                RuntimeDependencies.Add(DefaultVoice);
            }

            string DefaultVoiceJson = Path.Combine(PluginDir, "Resources", "Voices", "en_US-lessac-small.onnx.json");
            if (File.Exists(DefaultVoiceJson))
            {
                RuntimeDependencies.Add(DefaultVoiceJson);
            }

            string PiperLicense = Path.Combine(PluginDir, "Resources", "ThirdPartyNotices", "piper_LICENSE.txt");
            if (File.Exists(PiperLicense))
            {
                RuntimeDependencies.Add(PiperLicense);
            }
        }
    }
}
