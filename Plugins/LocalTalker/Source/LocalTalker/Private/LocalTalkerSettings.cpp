#include "LocalTalkerSettings.h"

#include "AudioCaptureCore.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

ULocalTalkerSettings::ULocalTalkerSettings()
{
    // Seed a few default IDs for Qwen CustomVoice models.
    // Users can add their own entries (speaker ids and/or externally generated prompt files).
    if (Voices.Num() == 0)
    {
        auto AddVoice = [&](const TCHAR* Id, const TCHAR* Speaker, const TCHAR* Instruction)
        {
            FLocalTalkVoiceOption V;
            V.Id = Id;
            V.QwenSpeaker = Speaker;
            V.QwenInstruction = Instruction;
            Voices.Add(V);
        };

        AddVoice(TEXT("Vivian"), TEXT("Vivian"), TEXT(""));
        AddVoice(TEXT("Cherry"), TEXT("Cherry"), TEXT(""));
        AddVoice(TEXT("Ethan"), TEXT("Ethan"), TEXT(""));
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

TArray<FString> ULocalTalkerSettings::GetMicInputDeviceOptions() const
{
    TArray<FString> Out;
    TArray<Audio::FCaptureDeviceInfo> Devices;

    Audio::FAudioCapture Capture;
    Capture.GetCaptureDevicesAvailable(Devices);

    for (const Audio::FCaptureDeviceInfo& Device : Devices)
    {
        if (!Device.DeviceName.IsEmpty())
        {
            Out.AddUnique(Device.DeviceName);
        }
    }

    Out.Sort();
    return Out;
}
