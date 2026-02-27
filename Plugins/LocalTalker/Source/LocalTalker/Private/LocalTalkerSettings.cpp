#include "LocalTalkerSettings.h"

#include "AudioCaptureCore.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

ULocalTalkerSettings::ULocalTalkerSettings()
{
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
