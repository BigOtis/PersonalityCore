#include "AutoChatMicSelectorWidget.h"

#include "AutoChatVoiceInputComponent.h"

#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/ComboBoxString.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Blueprint/WidgetTree.h"
#include "Engine/World.h"
#include "TimerManager.h"

TSharedRef<SWidget> UAutoChatMicSelectorWidget::RebuildWidget()
{
    if (!WidgetTree)
    {
        WidgetTree = NewObject<UWidgetTree>(this, TEXT("WidgetTree"));
    }

    UVerticalBox* RootBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("RootVBox"));
    WidgetTree->RootWidget = RootBox;

    // Row: voice activity indicator + current mic name.
    UHorizontalBox* HeaderRow = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), TEXT("HeaderRow"));
    RootBox->AddChildToVerticalBox(HeaderRow);

    VoiceActivityIndicator = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("VoiceActivityIndicator"));
    VoiceActivityIndicator->SetBrushColor(FLinearColor::Gray);
    VoiceActivityIndicator->SetPadding(FMargin(4.0f));
    if (UHorizontalBoxSlot* IndicatorSlot = HeaderRow->AddChildToHorizontalBox(VoiceActivityIndicator))
    {
        IndicatorSlot->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f));
    }

    CurrentMicText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("CurrentMicText"));
    CurrentMicText->SetText(FText::FromString(TEXT("Mic: <Default>")));
    HeaderRow->AddChildToHorizontalBox(CurrentMicText);

    // Label + combo for mic selection.
    UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("MicLabel"));
    Label->SetText(FText::FromString(TEXT("Select Microphone")));
    RootBox->AddChildToVerticalBox(Label);

    MicCombo = WidgetTree->ConstructWidget<UComboBoxString>(UComboBoxString::StaticClass(), TEXT("MicCombo"));
    RootBox->AddChildToVerticalBox(MicCombo);

    ApplyButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("ApplyButton"));
    if (UTextBlock* ButtonText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("ApplyButtonText")))
    {
        ButtonText->SetText(FText::FromString(TEXT("Apply Microphone")));
        ApplyButton->AddChild(ButtonText);
    }
    RootBox->AddChildToVerticalBox(ApplyButton);

    StatusText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("StatusText"));
    StatusText->SetText(FText::FromString(TEXT("Select a microphone and apply.")));
    RootBox->AddChildToVerticalBox(StatusText);

    return Super::RebuildWidget();
}

void UAutoChatMicSelectorWidget::NativeConstruct()
{
    Super::NativeConstruct();

    if (ApplyButton)
    {
        ApplyButton->OnClicked.AddUniqueDynamic(this, &UAutoChatMicSelectorWidget::HandleApplyClicked);
    }

    RefreshMicrophoneList();
    UpdateCurrentMicLabel();
}

void UAutoChatMicSelectorWidget::RefreshMicrophoneList()
{
    if (!MicCombo)
    {
        return;
    }

    MicCombo->ClearOptions();
    MicCombo->AddOption(TEXT("Default (System)"));

    if (!VoiceInputComponent)
    {
        SetStatus(TEXT("Voice input component is not set."));
        MicCombo->SetSelectedOption(TEXT("Default (System)"));
        return;
    }

    const TArray<FString> Devices = VoiceInputComponent->GetAvailableMicrophones();
    for (const FString& Device : Devices)
    {
        if (!Device.IsEmpty())
        {
            MicCombo->AddOption(Device);
        }
    }

    if (VoiceInputComponent->MicInputDeviceMode == ELocalTalkMicInputDeviceMode::NamedDevice &&
        !VoiceInputComponent->MicInputDeviceName.IsEmpty())
    {
        MicCombo->SetSelectedOption(VoiceInputComponent->MicInputDeviceName);
    }
    else
    {
        MicCombo->SetSelectedOption(TEXT("Default (System)"));
    }

    UpdateCurrentMicLabel();
}

void UAutoChatMicSelectorWidget::HandleApplyClicked()
{
    if (!VoiceInputComponent)
    {
        SetStatus(TEXT("Cannot apply: voice input component is not set."));
        return;
    }

    const FString Selected = MicCombo ? MicCombo->GetSelectedOption() : FString();
    if (Selected.IsEmpty() || Selected.Equals(TEXT("Default (System)"), ESearchCase::IgnoreCase))
    {
        VoiceInputComponent->UseDefaultMicrophone();
        SetStatus(TEXT("Using system default microphone."));
        return;
    }

    VoiceInputComponent->UseNamedMicrophone(Selected);
    SetStatus(FString::Printf(TEXT("Using microphone: %s"), *Selected));
}

void UAutoChatMicSelectorWidget::SetStatus(const FString& Message)
{
    if (StatusText)
    {
        StatusText->SetText(FText::FromString(Message));
    }
}

void UAutoChatMicSelectorWidget::UpdateCurrentMicLabel()
{
    if (!CurrentMicText)
    {
        return;
    }

    FString Label = TEXT("Mic: <None>");
    if (VoiceInputComponent)
    {
        if (VoiceInputComponent->MicInputDeviceMode == ELocalTalkMicInputDeviceMode::NamedDevice &&
            !VoiceInputComponent->MicInputDeviceName.IsEmpty())
        {
            Label = FString::Printf(TEXT("Mic: %s"), *VoiceInputComponent->MicInputDeviceName);
        }
        else
        {
            Label = TEXT("Mic: Default (System)");
        }
    }

    CurrentMicText->SetText(FText::FromString(Label));
}

void UAutoChatMicSelectorWidget::NotifyVoiceActivity()
{
    if (VoiceActivityIndicator)
    {
        VoiceActivityIndicator->SetBrushColor(FLinearColor::Green);
    }
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(VoiceIndicatorTimer);
        World->GetTimerManager().SetTimer(
            VoiceIndicatorTimer,
            this,
            &UAutoChatMicSelectorWidget::ResetVoiceIndicator,
            0.25f,
            false);
    }
}

void UAutoChatMicSelectorWidget::ResetVoiceIndicator()
{
    if (VoiceActivityIndicator)
    {
        VoiceActivityIndicator->SetBrushColor(FLinearColor::Gray);
    }
}

