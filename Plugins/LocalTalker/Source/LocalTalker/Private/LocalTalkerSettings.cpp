#include "LocalTalkerSettings.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

ULocalTalkerSettings::ULocalTalkerSettings()
{
    // Provide a default voice option that matches the bundled files.
    // (The file may be staged later; this just seeds the dropdown.)
    if (Voices.Num() == 0)
    {
        FString BaseDir;
        if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LocalTalker")))
        {
            BaseDir = Plugin->GetBaseDir();
        }

        FLocalTalkVoiceOption V;
        V.Id = TEXT("en_US-lessac-small");
        if (!BaseDir.IsEmpty())
        {
            V.VoiceOnnxPath = FPaths::Combine(BaseDir, TEXT("Resources/Voices/en_US-lessac-small.onnx"));
            V.VoiceJsonPath = FPaths::Combine(BaseDir, TEXT("Resources/Voices/en_US-lessac-small.onnx.json"));
        }
        Voices.Add(V);
    }
}

FName ULocalTalkerSettings::GetCategoryName() const
{
    return TEXT("Plugins");
}

FName ULocalTalkerSettings::GetSectionName() const
{
    return TEXT("LocalTalker");
}

TArray<FString> ULocalTalkerSettings::GetAvailableModelOptions() const
{
    TArray<FString> Out;

    FString BaseDir;
    if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LocalTalker")))
    {
        BaseDir = Plugin->GetBaseDir();
    }
    if (BaseDir.IsEmpty())
    {
        return Out;
    }

    const FString ModelsDir = FPaths::Combine(BaseDir, TEXT("Resources/Models"));
    const FString Pattern = FPaths::Combine(ModelsDir, TEXT("*.gguf"));

    IFileManager& FM = IFileManager::Get();
    FM.FindFiles(Out, *Pattern, true, false);
    Out.Sort();
    return Out;
}
