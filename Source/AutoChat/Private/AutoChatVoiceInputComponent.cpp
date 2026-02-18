#include "AutoChatVoiceInputComponent.h"

#include "AutoChatMicSelectorWidget.h"
#include "LocalCharacterComponent.h"
#include "LocalTalkConversationSubsystem.h"
#include "LocalTalkerSettings.h"

#include "Blueprint/UserWidget.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

UAutoChatVoiceInputComponent::UAutoChatVoiceInputComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    MicSelectorWidgetClass = UAutoChatMicSelectorWidget::StaticClass();

    if (const ULocalTalkerSettings* Settings = GetDefault<ULocalTalkerSettings>())
    {
        MicInputDeviceMode = Settings->MicInputDeviceMode;
        MicInputDeviceName = Settings->MicInputDeviceName;
    }
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
}

void UAutoChatVoiceInputComponent::UseNamedMicrophone(const FString& DeviceName)
{
    MicInputDeviceMode = ELocalTalkMicInputDeviceMode::NamedDevice;
    MicInputDeviceName = DeviceName;
}

bool UAutoChatVoiceInputComponent::StartVoiceCaptureAndTranscribe()
{
    const FString Error = TEXT("No STT backend is wired yet. Capture your transcript externally and call SubmitRecognizedSpeech.");
    OnVoiceError.Broadcast(Error);
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

        Sub->RequestTurn(Target, Text);
        ++RoutedCount;
    }

    if (RoutedCount > 0)
    {
        OnTranscriptSubmitted.Broadcast(Text, RoutedCount);
        return true;
    }

    OnVoiceError.Broadcast(TEXT("Failed to route transcript to any nearby AI."));
    return false;
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
