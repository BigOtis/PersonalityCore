#include "AutoChatMicSelectorWidget.h"

#include "AutoChatVoiceInputComponent.h"

#include "Components/Button.h"
#include "Components/ComboBoxString.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Blueprint/WidgetTree.h"

TSharedRef<SWidget> UAutoChatMicSelectorWidget::RebuildWidget()
{
    if (!WidgetTree)
    {
        WidgetTree = NewObject<UWidgetTree>(this, TEXT("WidgetTree"));
    }

    UVerticalBox* RootBox = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("RootVBox"));
    WidgetTree->RootWidget = RootBox;

    UTextBlock* Label = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("MicLabel"));
    Label->SetText(FText::FromString(TEXT("Microphone")));
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

