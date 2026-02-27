#include "AutoChatVoiceInputComponent.h"

#include "AutoChatMicSelectorWidget.h"
#include "LocalCharacterComponent.h"
#include "LocalTalkConversationSubsystem.h"
#include "LocalTalkerSettings.h"

#include "Blueprint/UserWidget.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "SubtitleManager.h"
#include "Async/Async.h"

DEFINE_LOG_CATEGORY_STATIC(LogAutoChatVoice, Log, All);

UAutoChatVoiceInputComponent::UAutoChatVoiceInputComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    MicSelectorWidgetClass = UAutoChatMicSelectorWidget::StaticClass();

    if (const ULocalTalkerSettings* Settings = GetDefault<ULocalTalkerSettings>())
    {
        // Only apply project defaults if this component hasn't been explicitly
        // configured (e.g., via blueprint instance settings).
        const bool bIsDefaultMode = (MicInputDeviceMode == ELocalTalkMicInputDeviceMode::DefaultSystem);
        const bool bHasCustomName = !MicInputDeviceName.IsEmpty();
        if (bIsDefaultMode && !bHasCustomName)
        {
            MicInputDeviceMode = Settings->MicInputDeviceMode;
            MicInputDeviceName = Settings->MicInputDeviceName;
        }
    }

    if (PlayerSubtitleSpeakerName.IsEmpty())
    {
        PlayerSubtitleSpeakerName = TEXT("You");
    }
}

void UAutoChatVoiceInputComponent::BeginPlay()
{
    Super::BeginPlay();

    const FString OwnerName = GetOwner() ? GetOwner()->GetName() : TEXT("<NoOwner>");

    // Log configured mic mode/name on startup.
    const UEnum* ModeEnum = StaticEnum<ELocalTalkMicInputDeviceMode>();
    const FString ModeStr = ModeEnum
        ? ModeEnum->GetNameStringByValue(static_cast<int64>(MicInputDeviceMode))
        : TEXT("<Unknown>");

    UE_LOG(LogAutoChatVoice, Log,
        TEXT("[AutoChatVoiceInput] BeginPlay: Owner=%s MicMode=%s DeviceName=\"%s\""),
        *OwnerName,
        *ModeStr,
        MicInputDeviceName.IsEmpty() ? TEXT("<Default>") : *MicInputDeviceName);

    // Also log the available microphone devices from LocalTalker.
    if (const ULocalTalkerSettings* Settings = GetDefault<ULocalTalkerSettings>())
    {
        const TArray<FString> Devices = Settings->GetMicInputDeviceOptions();
        FString Joined = Devices.Num() > 0 ? FString::Join(Devices, TEXT(", ")) : TEXT("<None>");

        UE_LOG(LogAutoChatVoice, Log,
            TEXT("[AutoChatVoiceInput] Available microphones (%d): %s"),
            Devices.Num(),
            *Joined);

        if (MicInputDeviceMode == ELocalTalkMicInputDeviceMode::NamedDevice && !MicInputDeviceName.IsEmpty())
        {
            bool bFound = false;
            for (const FString& Dev : Devices)
            {
                if (Dev.Equals(MicInputDeviceName, ESearchCase::IgnoreCase))
                {
                    bFound = true;
                    break;
                }
            }

            if (bFound)
            {
                UE_LOG(LogAutoChatVoice, Log,
                    TEXT("[AutoChatVoiceInput] NamedDevice will target microphone \"%s\" (found in available list)."),
                    *MicInputDeviceName);
            }
            else
            {
                UE_LOG(LogAutoChatVoice, Warning,
                    TEXT("[AutoChatVoiceInput] WARNING: NamedDevice \"%s\" not found in available microphones. Any STT backend may fall back to the OS default input device."),
                    *MicInputDeviceName);
            }
        }
    }

    // Automatically show the mic selector / status UI so the player can see
    // which microphone is selected and whether voice input is being detected.
    ShowMicSelectorUI();

    // Start a lightweight mic level monitor so we can confirm that audio input
    // is flowing from the selected device (even before STT is wired up).
    StartMicCapture();
}

void UAutoChatVoiceInputComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    StopMicCapture();
    Super::EndPlay(EndPlayReason);
}

TArray<FString> UAutoChatVoiceInputComponent::GetAvailableMicrophones() const
{
    if (const ULocalTalkerSettings* Settings = GetDefault<ULocalTalkerSettings>())
    {
        return Settings->GetMicInputDeviceOptions();
    }
    return TArray<FString>();
}

void UAutoChatVoiceInputComponent::UseDefaultMicrophone()
{
    MicInputDeviceMode = ELocalTalkMicInputDeviceMode::DefaultSystem;
    MicInputDeviceName.Reset();

    const FString OwnerName = GetOwner() ? GetOwner()->GetName() : TEXT("<NoOwner>");
    UE_LOG(LogAutoChatVoice, Log,
        TEXT("[AutoChatVoiceInput] %s selected DefaultSystem microphone"), *OwnerName);
}

void UAutoChatVoiceInputComponent::UseNamedMicrophone(const FString& DeviceName)
{
    MicInputDeviceMode = ELocalTalkMicInputDeviceMode::NamedDevice;
    MicInputDeviceName = DeviceName;

    const FString OwnerName = GetOwner() ? GetOwner()->GetName() : TEXT("<NoOwner>");
    UE_LOG(LogAutoChatVoice, Log,
        TEXT("[AutoChatVoiceInput] %s selected NamedDevice microphone \"%s\""),
        *OwnerName,
        *MicInputDeviceName);
}

bool UAutoChatVoiceInputComponent::StartVoiceCaptureAndTranscribe()
{
    const FString Error = TEXT("No STT backend is wired yet. Capture your transcript externally and call SubmitRecognizedSpeech.");
    OnVoiceError.Broadcast(Error);
    UE_LOG(LogAutoChatVoice, Warning,
        TEXT("[AutoChatVoiceInput] StartVoiceCaptureAndTranscribe called, but no STT backend is integrated. "
             "This function is currently a stub; use SubmitRecognizedSpeech / SubmitSimulatedVoiceInput with your own STT pipeline."));
    return false;
}

TArray<ULocalCharacterComponent*> UAutoChatVoiceInputComponent::FindNearbyAIs(float OverrideRange) const
{
    TArray<ULocalCharacterComponent*> Result;

    const AActor* Owner = GetOwner();
    UWorld* World = GetWorld();
    if (!Owner || !World)
    {
        return Result;
    }

    ULocalTalkConversationSubsystem* Sub = World->GetSubsystem<ULocalTalkConversationSubsystem>();
    if (!Sub)
    {
        return Result;
    }

    const FVector PlayerLoc = Owner->GetActorLocation();
    const float MaxRange = (OverrideRange > 0.0f) ? OverrideRange : MaxInteractionRange;
    const float MaxRangeSq = (MaxRange > 0.0f) ? (MaxRange * MaxRange) : TNumericLimits<float>::Max();

    for (ULocalCharacterComponent* AI : Sub->GetRegisteredTalkers())
    {
        if (!AI || !AI->GetOwner() || AI->GetOwner() == Owner)
        {
            continue;
        }

        const float DistSq = FVector::DistSquared(PlayerLoc, AI->GetOwner()->GetActorLocation());
        const float Hearing = FMath::Max(0.0f, AI->GetHearingRadius());
        if (Hearing <= 0.0f)
        {
            continue;
        }

        const float HearingSq = Hearing * Hearing;
        if (DistSq <= HearingSq && DistSq <= MaxRangeSq)
        {
            Result.Add(AI);
        }
    }

    Result.Sort([&](const ULocalCharacterComponent& A, const ULocalCharacterComponent& B)
    {
        const float DA = FVector::DistSquared(PlayerLoc, A.GetOwner()->GetActorLocation());
        const float DB = FVector::DistSquared(PlayerLoc, B.GetOwner()->GetActorLocation());
        return DA < DB;
    });

    return Result;
}

void UAutoChatVoiceInputComponent::ApplyMicSelectionToAI(ULocalCharacterComponent* AI) const
{
    if (!AI)
    {
        return;
    }

    AI->bUseProjectSettingsMicInput = false;
    AI->MicInputDeviceMode = MicInputDeviceMode;
    AI->MicInputDeviceName = MicInputDeviceName;
}

bool UAutoChatVoiceInputComponent::SubmitRecognizedSpeech(const FString& Transcript)
{
    const FString Text = Transcript.TrimStartAndEnd();
    if (Text.IsEmpty())
    {
        OnVoiceError.Broadcast(TEXT("Transcript is empty."));
        return false;
    }

    // At this point we received non-empty text from an STT backend.
    {
        const FString OwnerName = GetOwner() ? GetOwner()->GetName() : TEXT("<NoOwner>");
        FString Preview = Text;
        const int32 MaxChars = 120;
        if (Preview.Len() > MaxChars)
        {
            Preview = Preview.Left(MaxChars) + TEXT("...");
        }

        UE_LOG(LogAutoChatVoice, Log,
            TEXT("[AutoChatVoiceInput] Voice input detected from %s: \"%s\""),
            *OwnerName,
            *Preview);
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        OnVoiceError.Broadcast(TEXT("World is not available."));
        return false;
    }

    ULocalTalkConversationSubsystem* Sub = World->GetSubsystem<ULocalTalkConversationSubsystem>();
    if (!Sub)
    {
        OnVoiceError.Broadcast(TEXT("LocalTalk conversation subsystem is not available."));
        return false;
    }

    TArray<ULocalCharacterComponent*> Targets = FindNearbyAIs();
    if (Targets.Num() == 0)
    {
        OnVoiceError.Broadcast(TEXT("No AI talkers are within hearing range."));
        UE_LOG(LogAutoChatVoice, Warning,
            TEXT("[AutoChatVoiceInput] No AI talkers in range for transcript \"%s\""),
            *Text);
        return false;
    }

    int32 RoutedCount = 0;
    const int32 NumToRoute = bBroadcastToAllNearby ? Targets.Num() : 1;
    for (int32 i = 0; i < NumToRoute; ++i)
    {
        ULocalCharacterComponent* Target = Targets[i];
        if (!Target)
        {
            continue;
        }

        if (bApplyMicSelectionToTargetAI)
        {
            ApplyMicSelectionToAI(Target);
        }

        const FString TargetOwnerName = Target->GetOwner() ? Target->GetOwner()->GetName() : TEXT("<NoOwner>");
        UE_LOG(LogAutoChatVoice, Log,
            TEXT("[AutoChatVoiceInput] Routing transcript to AI \"%s\" at index %d/%d"),
            *TargetOwnerName,
            i + 1,
            NumToRoute);

        Sub->RequestTurn(Target, Text);
        ++RoutedCount;
    }

    if (RoutedCount > 0)
    {
        OnTranscriptSubmitted.Broadcast(Text, RoutedCount);
        ShowPlayerSubtitle(Text, RoutedCount);

        // Ping the mic selector widget (if present) so it can show a visual
        // cue that voice input was received.
        if (UAutoChatMicSelectorWidget* MicWidget = Cast<UAutoChatMicSelectorWidget>(ActiveMicSelectorWidget))
        {
            MicWidget->NotifyVoiceActivity();
        }

        return true;
    }

    OnVoiceError.Broadcast(TEXT("Failed to route transcript to any nearby AI."));
    return false;
}

void UAutoChatVoiceInputComponent::ShowPlayerSubtitle(const FString& Transcript, int32 NumTargets)
{
    if (!bShowPlayerSubtitles)
    {
        return;
    }

    const FString SpeakerLabel = PlayerSubtitleSpeakerName.IsEmpty() ? TEXT("You") : PlayerSubtitleSpeakerName;
    const FString Line = FString::Printf(TEXT("%s: %s"), *SpeakerLabel, *Transcript);

    // Rough heuristic: scale subtitle time with text length, but clamp to a reasonable range.
    const float Heuristic = FMath::Max(0.01f, PlayerSubtitleSecondsPerChar);
    const float DurationSec = FMath::Clamp(Transcript.Len() * Heuristic, 2.0f, 12.0f);

    UWorld* World = GetWorld();
    if (!World)
    {
        if (GEngine)
        {
            GEngine->AddOnScreenDebugMessage(
                /*Key*/ (uint64)this,
                DurationSec,
                FColor::Green,
                Line);
        }
        return;
    }

    // Use UE's SubtitleManager so player speech appears like other subtitles.
    const PTRINT SubtitleId = (PTRINT)this;

    TArray<FSubtitleCue> Cues;
    FSubtitleCue Cue;
    Cue.Text = FText::FromString(Line);
    Cue.Time = 0.0f;
    Cues.Add(Cue);

    FSubtitleManager::GetSubtitleManager()->QueueSubtitles(
        SubtitleId,
        /*Priority*/ 1000.0f,
        /*bManualWordWrap*/ false,
        /*bSingleLine*/ true,
        DurationSec,
        Cues,
        /*InStartTime*/ 0.0f,
        World->GetAudioTimeSeconds());
}

bool UAutoChatVoiceInputComponent::ShowMicSelectorUI(APlayerController* OwningPlayer)
{
    if (ActiveMicSelectorWidget)
    {
        return true;
    }

    if (!MicSelectorWidgetClass)
    {
        OnVoiceError.Broadcast(TEXT("MicSelectorWidgetClass is not set."));
        return false;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        OnVoiceError.Broadcast(TEXT("World is not available."));
        return false;
    }

    APlayerController* PC = OwningPlayer ? OwningPlayer : World->GetFirstPlayerController();
    UUserWidget* Widget = CreateWidget<UUserWidget>(World, MicSelectorWidgetClass, NAME_None);
    if (!Widget)
    {
        OnVoiceError.Broadcast(TEXT("Failed to create mic selector widget."));
        return false;
    }

    if (UAutoChatMicSelectorWidget* MicWidget = Cast<UAutoChatMicSelectorWidget>(Widget))
    {
        MicWidget->VoiceInputComponent = this;
        MicWidget->RefreshMicrophoneList();
    }

    Widget->AddToViewport();
    if (PC)
    {
        PC->bShowMouseCursor = true;
    }

    ActiveMicSelectorWidget = Widget;
    return true;
}

void UAutoChatVoiceInputComponent::HideMicSelectorUI()
{
    if (!ActiveMicSelectorWidget)
    {
        return;
    }

    ActiveMicSelectorWidget->RemoveFromParent();
    ActiveMicSelectorWidget = nullptr;
}

void UAutoChatVoiceInputComponent::StartMicCapture()
{
    if (bMicCaptureActive)
    {
        return;
    }

    TArray<Audio::FCaptureDeviceInfo> Devices;
    MicCapture.GetCaptureDevicesAvailable(Devices);

    int32 TargetDeviceIndex = INDEX_NONE; // default system
    FString TargetDeviceName = TEXT("Default (System)");

    if (MicInputDeviceMode == ELocalTalkMicInputDeviceMode::NamedDevice && !MicInputDeviceName.IsEmpty())
    {
        for (int32 Index = 0; Index < Devices.Num(); ++Index)
        {
            if (Devices[Index].DeviceName.Equals(MicInputDeviceName, ESearchCase::IgnoreCase))
            {
                TargetDeviceIndex = Index;
                TargetDeviceName = Devices[Index].DeviceName;
                break;
            }
        }
    }
    else if (Devices.Num() > 0)
    {
        TargetDeviceIndex = 0;
        TargetDeviceName = Devices[0].DeviceName;
    }

    Audio::FAudioCaptureDeviceParams Params;
    Params.DeviceIndex = TargetDeviceIndex;

    const int32 FramesPerBuffer = 1024;

    if (!MicCapture.OpenAudioCaptureStream(
        Params,
        [this](const void* InAudio, int32 NumFrames, int32 NumChannels, int32 SampleRate, double StreamTime, bool bOverflow)
        {
            const float* FloatAudio = static_cast<const float*>(InAudio);
            OnAudioCapture(FloatAudio, NumFrames, NumChannels, StreamTime, bOverflow);
        },
        FramesPerBuffer))
    {
        UE_LOG(LogAutoChatVoice, Warning,
            TEXT("[AutoChatVoiceInput] Failed to open mic capture stream (DeviceIndex=%d, RequestedName=\"%s\")."),
            TargetDeviceIndex,
            *MicInputDeviceName);
        return;
    }

    if (!MicCapture.StartStream())
    {
        UE_LOG(LogAutoChatVoice, Warning,
            TEXT("[AutoChatVoiceInput] Failed to start mic capture stream (DeviceIndex=%d, Name=\"%s\")."),
            TargetDeviceIndex,
            *TargetDeviceName);
        MicCapture.CloseStream();
        return;
    }

    bMicCaptureActive = true;

    UE_LOG(LogAutoChatVoice, Log,
        TEXT("[AutoChatVoiceInput] Mic capture started on device index %d (\"%s\")."),
        TargetDeviceIndex,
        *TargetDeviceName);
}

void UAutoChatVoiceInputComponent::StopMicCapture()
{
    if (!bMicCaptureActive)
    {
        return;
    }

    bMicCaptureActive = false;

    if (MicCapture.IsStreamOpen())
    {
        if (MicCapture.IsCapturing())
        {
            MicCapture.StopStream();
        }
        MicCapture.CloseStream();
    }

    UE_LOG(LogAutoChatVoice, Log, TEXT("[AutoChatVoiceInput] Mic capture stopped."));
}

void UAutoChatVoiceInputComponent::OnAudioCapture(const float* AudioData, int32 NumFrames, int32 NumChannels, double StreamTime, bool bOverflow)
{
    if (!bMicCaptureActive || !AudioData || NumFrames <= 0 || NumChannels <= 0)
    {
        return;
    }

    const int32 NumSamples = NumFrames * NumChannels;
    if (NumSamples <= 0)
    {
        return;
    }

    double SumSq = 0.0;
    for (int32 i = 0; i < NumSamples; ++i)
    {
        const double S = (double)AudioData[i];
        SumSq += S * S;
    }

    const double MeanSq = SumSq / (double)NumSamples;
    const float Rms = (float)FMath::Sqrt((float)MeanSq);

    // Simple threshold to decide whether there's "voice-like" activity.
    static constexpr float ActivityThreshold = 0.01f;
    if (Rms < ActivityThreshold)
    {
        return;
    }

    // Hop to the game thread to ping the mic widget.
    AsyncTask(ENamedThreads::GameThread, [WeakThis = TWeakObjectPtr<UAutoChatVoiceInputComponent>(this)]()
    {
        UAutoChatVoiceInputComponent* Self = WeakThis.Get();
        if (!Self)
        {
            return;
        }

        if (UAutoChatMicSelectorWidget* MicWidget = Cast<UAutoChatMicSelectorWidget>(Self->ActiveMicSelectorWidget))
        {
            MicWidget->NotifyVoiceActivity();
        }
    });
}
