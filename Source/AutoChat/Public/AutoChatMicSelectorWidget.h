#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "AutoChatMicSelectorWidget.generated.h"

class UButton;
class UComboBoxString;
class UTextBlock;
class UAutoChatVoiceInputComponent;

/**
 * Minimal runtime mic selector UI.
 * Built entirely in C++ so it can be used without authoring widget assets.
 */
UCLASS()
class AUTOCHAT_API UAutoChatMicSelectorWidget : public UUserWidget
{
    GENERATED_BODY()

public:
    /** Voice component this widget controls. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="AutoChat")
    TObjectPtr<UAutoChatVoiceInputComponent> VoiceInputComponent = nullptr;

    /** Refreshes mic options from the voice component. */
    UFUNCTION(BlueprintCallable, Category="AutoChat")
    void RefreshMicrophoneList();

protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void NativeConstruct() override;

private:
    UPROPERTY(Transient)
    TObjectPtr<UComboBoxString> MicCombo = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UButton> ApplyButton = nullptr;

    UPROPERTY(Transient)
    TObjectPtr<UTextBlock> StatusText = nullptr;

    UFUNCTION()
    void HandleApplyClicked();

    void SetStatus(const FString& Message);
};

