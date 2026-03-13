#include "AutoChatVoiceInputComponent.h"

#include "AutoChatMicSelectorWidget.h"
#include "LocalCharacterComponent.h"
#include "LocalPlayerInteractionComponent.h"
#include "LocalTalkConversationSubsystem.h"
#include "LocalTalkerSettings.h"

#include "Blueprint/UserWidget.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "SubtitleManager.h"
#include "Async/Async.h"

DEFINE_LOG_CATEGORY_STATIC(LogAutoChatVoice, Log, All);

namespace
{
void LocalVoiceTokenize(const FString& InText, TArray<FString>& OutTokens)
{
    OutTokens.Reset();

    FString Current;
    Current.Reserve(24);

    auto FlushCurrent = [&]()
    {
        if (Current.Len() >= 2)
        {
            OutTokens.Add(Current);
        }
        Current.Reset();
    };

    for (int32 i = 0; i < InText.Len(); ++i)
    {
        const TCHAR C = InText[i];
        if (FChar::IsAlpha(C) || FChar::IsDigit(C))
        {
            Current.AppendChar(FChar::ToLower(C));
        }
        else
        {
            FlushCurrent();
        }
    }
    FlushCurrent();
}

float LocalVoiceTokenOverlapScore(const TArray<FString>& A, const TArray<FString>& B)
{
    if (A.Num() == 0 || B.Num() == 0)
    {
        return 0.0f;
    }

    TMap<FString, int32> CountA;
    TMap<FString, int32> CountB;
    for (const FString& T : A)
    {
        CountA.FindOrAdd(T)++;
    }
    for (const FString& T : B)
    {
        CountB.FindOrAdd(T)++;
    }

    int32 Common = 0;
    for (const TPair<FString, int32>& Pair : CountA)
    {
        if (const int32* BCount = CountB.Find(Pair.Key))
        {
            Common += FMath::Min(Pair.Value, *BCount);
        }
    }

    const int32 Den = FMath::Min(A.Num(), B.Num());
    if (Den <= 0)
    {
        return 0.0f;
    }
    return (float)Common / (float)Den;
}

float LocalVoiceUniqueTokenRatio(const TArray<FString>& Tokens, int32& OutDominantCount)
{
    OutDominantCount = 0;
    if (Tokens.Num() == 0)
    {
        return 1.0f;
    }

    TMap<FString, int32> Counts;
    for (const FString& Tok : Tokens)
    {
        const int32 NewCount = ++Counts.FindOrAdd(Tok);
        OutDominantCount = FMath::Max(OutDominantCount, NewCount);
    }
    return (float)Counts.Num() / (float)Tokens.Num();
}
}

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
    UE_LOG(LogAutoChatVoice, Log,
        TEXT("[AutoChatVoiceInput] STT mode: alwaysOn=%d startRms=%.4f continueRms=%.4f startHold=%.2fs bargeInRms=%.4f bargeMinActive=%.2fs bargeMinLoud=%.2fs bargeVeryLoud=%.3f bargeForce=%.2fs submitGrace=%.2fs silenceEnd=%.2fs minSpeech=%.2fs maxSpeech=%.2fs minActive=%.2fs minActiveRatio=%.2f bargeHold=%.2fs postSegHold=%.2fs activeSegHold=%.2fs activeSegRefresh=%.2fs postTextHold=%.2fs fenceMax=%.2fs fenceOnSegStart=%d bargeNeedAudible=%d dropAIAudibleSeg=%d adaptiveNoise=%d noiseFloor=[%.4f..%.4f] startMul=%.2f contMul=%.2f bargeMul=%.2f maxEffStart=%.4f maxEffCont=%.4f echoGuard=%d echoThr=%.2f lowQual=%d minUnique=%.2f rotateSingle=%d"),
        bAlwaysOnAutoTranscribe ? 1 : 0,
        AutoTranscribeStartRmsThreshold,
        AutoTranscribeContinueRmsThreshold,
        AutoTranscribeStartHoldSeconds,
        BargeInMinRmsThreshold,
        BargeInMinActiveSpeechSeconds,
        BargeInMinLoudSeconds,
        BargeInVeryLoudRmsThreshold,
        BargeInForceInterruptActiveSeconds,
        PostTranscriptBargeInGraceSeconds,
        AutoTranscribeSilenceSeconds,
        AutoTranscribeMinSpeechSeconds,
        AutoTranscribeMaxSpeechSeconds,
        AutoTranscribeMinActiveSpeechSeconds,
        AutoTranscribeMinActiveRatio,
        BargeInPriorityHoldSeconds,
        PostSegmentPriorityHoldSeconds,
        ActiveSegmentPriorityHoldSeconds,
        ActiveSegmentPriorityRefreshSeconds,
        PostTranscriptPriorityHoldSeconds,
        PlayerSpeechFenceMaxSeconds,
        bFenceDuringAlwaysOnSegment ? 1 : 0,
        bBargeInRequiresAudibleAIVoice ? 1 : 0,
        bDropAIAudibleSegmentsWithoutBargeIn ? 1 : 0,
        bUseAdaptiveNoiseFloor ? 1 : 0,
        AdaptiveNoiseFloorMinRms,
        AdaptiveNoiseFloorMaxRms,
        AdaptiveStartThresholdMultiplier,
        AdaptiveContinueThresholdMultiplier,
        AdaptiveBargeInThresholdMultiplier,
        MaxEffectiveStartRmsThreshold,
        MaxEffectiveContinueRmsThreshold,
        bRejectLikelyNpcEchoTranscripts ? 1 : 0,
        NpcEchoSimilarityThreshold,
        bRejectLowQualityTranscripts ? 1 : 0,
        MinTranscriptUniqueTokenRatio,
        bRotateSingleTargetAcrossNearby ? 1 : 0);

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

    if (bPrewarmWhisperOnBeginPlay && EnsureWhisperBridge() && WhisperBridgeComponent)
    {
        WhisperBridgeComponent->PrimeWhisperWorkerAsync();
    }
}

void UAutoChatVoiceInputComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    EndPlayerSpeechFence(TEXT("end_play"));

    if (WhisperBridgeComponent)
    {
        WhisperBridgeComponent->OnWhisperTranscription.RemoveDynamic(this, &UAutoChatVoiceInputComponent::HandleWhisperTranscription);
        WhisperBridgeComponent->OnWhisperError.RemoveDynamic(this, &UAutoChatVoiceInputComponent::HandleWhisperError);
        if (bWhisperCaptureInProgress)
        {
            WhisperBridgeComponent->CancelMicrophoneCapture();
            bWhisperCaptureInProgress = false;
        }
    }
    bAutoSpeechSegmentActive = false;
    bAutoSpeechSegmentBargeInTriggered = false;
    bAutoSpeechSegmentHadAudibleAI = false;
    bAutoTranscribeRequestInFlight = false;
    AutoSpeechStartGateFrames = 0;
    AdaptiveNoiseFloorRms = 0.0f;
    AutoSpeechStartWorldSeconds = 0.0;
    AutoSpeechLastActiveWorldSeconds = 0.0;
    AutoSpeechSampleRate = 0;
    AutoSpeechNumChannels = 0;
    AutoSpeechTotalFrames = 0;
    AutoSpeechActiveFrames = 0;
    AutoSpeechLoudFrames = 0;
    AutoSpeechPcm16.Reset();
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

    if (bMicCaptureActive)
    {
        StopMicCapture();
        StartMicCapture();
    }
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

    if (bMicCaptureActive)
    {
        StopMicCapture();
        StartMicCapture();
    }
}

bool UAutoChatVoiceInputComponent::StartVoiceCaptureAndTranscribe()
{
    UE_LOG(LogAutoChatVoice, Log, TEXT("[AutoChatVoiceInput] StartVoiceCaptureAndTranscribe toggle invoked. Active=%d"),
        bWhisperCaptureInProgress ? 1 : 0);

    return bWhisperCaptureInProgress ? EndVoiceCaptureAndTranscribe() : BeginVoiceCapture();
}

bool UAutoChatVoiceInputComponent::BeginVoiceCapture()
{
    if (bWhisperCaptureInProgress)
    {
        UE_LOG(LogAutoChatVoice, Verbose, TEXT("[AutoChatVoiceInput] BeginVoiceCapture ignored; capture already active."));
        return true;
    }

    bAutoSpeechSegmentActive = false;
    bAutoSpeechSegmentBargeInTriggered = false;
    bAutoSpeechSegmentHadAudibleAI = false;
    AutoSpeechStartGateFrames = 0;
    AutoSpeechTotalFrames = 0;
    AutoSpeechActiveFrames = 0;
    AutoSpeechLoudFrames = 0;
    AutoSpeechPcm16.Reset();
    AdaptiveNoiseFloorRms = 0.0f;

    if (!EnsureWhisperBridge() || !WhisperBridgeComponent)
    {
        const FString Error = TEXT("Whisper bridge component is not available.");
        OnVoiceError.Broadcast(Error);
        UE_LOG(LogAutoChatVoice, Error, TEXT("[AutoChatVoiceInput] %s"), *Error);
        return false;
    }

    WhisperBridgeComponent->bUseProjectSettingsMicInput = false;
    WhisperBridgeComponent->MicInputDeviceMode = MicInputDeviceMode;
    WhisperBridgeComponent->MicInputDeviceName = MicInputDeviceName;
    BeginPlayerSpeechFence(PlayerSpeechFenceMaxSeconds, TEXT("begin_voice_capture"));

    if (bInterruptNearbyAIOnVoiceCaptureStart)
    {
        InterruptNearbyAIsForPlayerSpeech();
    }

    // Stop debug monitor while Whisper captures to avoid device contention on strict drivers.
    StopMicCapture();

    if (!WhisperBridgeComponent->StartMicrophoneCapture())
    {
        const FString Error = TEXT("Failed to start Whisper microphone capture.");
        OnVoiceError.Broadcast(Error);
        UE_LOG(LogAutoChatVoice, Warning, TEXT("[AutoChatVoiceInput] %s"), *Error);
        StartMicCapture();
        return false;
    }

    bWhisperCaptureInProgress = true;

    const UEnum* ModeEnum = StaticEnum<ELocalTalkMicInputDeviceMode>();
    const FString ModeStr = ModeEnum
        ? ModeEnum->GetNameStringByValue(static_cast<int64>(MicInputDeviceMode))
        : TEXT("<Unknown>");

    UE_LOG(LogAutoChatVoice, Log,
        TEXT("[AutoChatVoiceInput] Whisper capture started. MicMode=%s MicName=\"%s\""),
        *ModeStr,
        MicInputDeviceName.IsEmpty() ? TEXT("<Default>") : *MicInputDeviceName);
    return true;
}

bool UAutoChatVoiceInputComponent::EndVoiceCaptureAndTranscribe()
{
    if (!bWhisperCaptureInProgress)
    {
        UE_LOG(LogAutoChatVoice, Warning, TEXT("[AutoChatVoiceInput] EndVoiceCaptureAndTranscribe called with no active capture."));
        return false;
    }

    if (!EnsureWhisperBridge() || !WhisperBridgeComponent)
    {
        const FString Error = TEXT("Whisper bridge component is not available while stopping capture.");
        OnVoiceError.Broadcast(Error);
        UE_LOG(LogAutoChatVoice, Error, TEXT("[AutoChatVoiceInput] %s"), *Error);
        bWhisperCaptureInProgress = false;
        StartMicCapture();
        EndPlayerSpeechFence(TEXT("ptt_stop_no_bridge"));
        return false;
    }

    const bool bRequested = WhisperBridgeComponent->StopMicrophoneCaptureAndTranscribe(
        /*bSendToNearestAI*/ false,
        MaxInteractionRange);
    if (!bRequested)
    {
        bWhisperCaptureInProgress = false;
        StartMicCapture();
        const FString Error = TEXT("Failed to stop Whisper capture and request transcription.");
        OnVoiceError.Broadcast(Error);
        UE_LOG(LogAutoChatVoice, Warning, TEXT("[AutoChatVoiceInput] %s"), *Error);
        EndPlayerSpeechFence(TEXT("ptt_stop_request_failed"));
        return false;
    }

    UE_LOG(LogAutoChatVoice, Log,
        TEXT("[AutoChatVoiceInput] Whisper capture stopped; transcription requested (range=%.1f broadcast=%d)."),
        MaxInteractionRange,
        bBroadcastToAllNearby ? 1 : 0);
    return true;
}

void UAutoChatVoiceInputComponent::CancelVoiceCapture()
{
    if (!bWhisperCaptureInProgress)
    {
        return;
    }

    if (EnsureWhisperBridge() && WhisperBridgeComponent)
    {
        WhisperBridgeComponent->CancelMicrophoneCapture();
    }

    bWhisperCaptureInProgress = false;
    StartMicCapture();
    EndPlayerSpeechFence(TEXT("cancel_voice_capture"));
    UE_LOG(LogAutoChatVoice, Log, TEXT("[AutoChatVoiceInput] Whisper capture canceled."));
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

bool UAutoChatVoiceInputComponent::HasNearbyAudibleAISpeech() const
{
    const TArray<ULocalCharacterComponent*> Nearby = FindNearbyAIs();
    for (ULocalCharacterComponent* AI : Nearby)
    {
        if (!AI)
        {
            continue;
        }
        if (AI->IsAudioPlaying())
        {
            return true;
        }
    }
    return false;
}

void UAutoChatVoiceInputComponent::InterruptNearbyAIsForPlayerSpeech()
{
    AActor* Owner = GetOwner();
    UWorld* World = GetWorld();
    if (!Owner || !World)
    {
        return;
    }

    ULocalTalkConversationSubsystem* Sub = World->GetSubsystem<ULocalTalkConversationSubsystem>();
    if (!Sub)
    {
        return;
    }

    const FVector PlayerLoc = Owner->GetActorLocation();
    const float Radius = FMath::Max(1.0f, MaxInteractionRange);

    if (bCancelQueuedTurnsOnVoiceCaptureStart)
    {
        Sub->CancelQueuedTurnsInProximity(PlayerLoc, Radius);
    }

    Sub->InterruptProximity(PlayerLoc, Radius);
    ApplyPlayerSpeechPriorityWindow(BargeInPriorityHoldSeconds, TEXT("barge_in_interrupt"));
    UE_LOG(LogAutoChatVoice, Log,
        TEXT("[AutoChatVoiceInput] Player barge-in: interrupted nearby AI speech (radius=%.1f, cancelQueued=%d, hold=%.2fs)."),
        Radius,
        bCancelQueuedTurnsOnVoiceCaptureStart ? 1 : 0,
        BargeInPriorityHoldSeconds);
}

void UAutoChatVoiceInputComponent::ApplyPlayerSpeechPriorityWindow(float HoldSeconds, const TCHAR* Reason)
{
    if (HoldSeconds <= 0.0f)
    {
        return;
    }

    AActor* Owner = GetOwner();
    UWorld* World = GetWorld();
    if (!Owner || !World)
    {
        return;
    }

    ULocalTalkConversationSubsystem* Sub = World->GetSubsystem<ULocalTalkConversationSubsystem>();
    if (!Sub)
    {
        return;
    }

    const FVector PlayerLoc = Owner->GetActorLocation();
    const float Radius = FMath::Max(1.0f, MaxInteractionRange);
    Sub->SetPlayerSpeechPriorityWindow(PlayerLoc, Radius, HoldSeconds);

    UE_LOG(LogAutoChatVoice, Log,
        TEXT("[AutoChatVoiceInput] Player priority window set (reason=%s radius=%.1f hold=%.2fs)."),
        Reason ? Reason : TEXT("unknown"),
        Radius,
        HoldSeconds);
}

void UAutoChatVoiceInputComponent::BeginPlayerSpeechFence(float MaxHoldSeconds, const TCHAR* Reason)
{
    if (MaxHoldSeconds <= 0.0f)
    {
        return;
    }

    AActor* Owner = GetOwner();
    UWorld* World = GetWorld();
    if (!Owner || !World)
    {
        return;
    }

    ULocalTalkConversationSubsystem* Sub = World->GetSubsystem<ULocalTalkConversationSubsystem>();
    if (!Sub)
    {
        return;
    }

    const FVector PlayerLoc = Owner->GetActorLocation();
    const float Radius = FMath::Max(1.0f, MaxInteractionRange);
    Sub->BeginPlayerSpeechFence(PlayerLoc, Radius, MaxHoldSeconds);
    UE_LOG(LogAutoChatVoice, Log,
        TEXT("[AutoChatVoiceInput] Player speech fence set (reason=%s radius=%.1f maxHold=%.2fs)."),
        Reason ? Reason : TEXT("unknown"),
        Radius,
        MaxHoldSeconds);
}

void UAutoChatVoiceInputComponent::EndPlayerSpeechFence(const TCHAR* Reason)
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    ULocalTalkConversationSubsystem* Sub = World->GetSubsystem<ULocalTalkConversationSubsystem>();
    if (!Sub)
    {
        return;
    }

    Sub->EndPlayerSpeechFence();
    UE_LOG(LogAutoChatVoice, Log,
        TEXT("[AutoChatVoiceInput] Player speech fence cleared (reason=%s)."),
        Reason ? Reason : TEXT("unknown"));
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
    const FString Text = NormalizeTranscriptForRouting(Transcript);
    if (Text.IsEmpty())
    {
        UE_LOG(LogAutoChatVoice, Log, TEXT("[AutoChatVoiceInput] Transcript ignored after normalization (empty/noise)."));
        return false;
    }

    FString SpamReason;
    if (IsTranscriptLikelyRepetitionSpam(Text, SpamReason))
    {
        UE_LOG(LogAutoChatVoice, Log,
            TEXT("[AutoChatVoiceInput] Transcript ignored by repetition guard: %s text=\"%s\""),
            *SpamReason,
            *Text.Left(120));
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
        UE_LOG(LogAutoChatVoice, Warning,
            TEXT("[AutoChatVoiceInput] No AI talkers in range for transcript \"%s\""),
            *Text);
        return false;
    }

    FString QualityReason;
    if (bRejectLowQualityTranscripts && IsTranscriptLikelyLowQuality(Text, QualityReason))
    {
        UE_LOG(LogAutoChatVoice, Log,
            TEXT("[AutoChatVoiceInput] Transcript ignored by low-quality guard: %s text=\"%s\""),
            *QualityReason,
            *Text.Left(120));
        return false;
    }

    FString EchoReason;
    if (bRejectLikelyNpcEchoTranscripts && IsTranscriptLikelyNpcEcho(Text, Sub, Targets, EchoReason))
    {
        UE_LOG(LogAutoChatVoice, Log,
            TEXT("[AutoChatVoiceInput] Transcript ignored by NPC-echo guard: %s text=\"%s\""),
            *EchoReason,
            *Text.Left(120));
        return false;
    }

    // At this point we have accepted a transcript for routing.
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

    // Re-assert player priority right before routing transcript so NPC auto-turns do not race ahead.
    if (bInterruptNearbyAIOnVoiceCaptureStart)
    {
        InterruptNearbyAIsForPlayerSpeech();
    }
    else if (bCancelQueuedTurnsOnVoiceCaptureStart)
    {
        if (AActor* Owner = GetOwner())
        {
            Sub->CancelQueuedTurnsInProximity(Owner->GetActorLocation(), FMath::Max(1.0f, MaxInteractionRange));
        }
    }
    ApplyPlayerSpeechPriorityWindow(PostTranscriptPriorityHoldSeconds, TEXT("transcript_submit"));

    int32 RoutedCount = 0;
    const int32 NumToRoute = bBroadcastToAllNearby ? Targets.Num() : 1;
    int32 StartIndex = 0;
    if (!bBroadcastToAllNearby && bRotateSingleTargetAcrossNearby && Targets.Num() > 1)
    {
        if (LastSingleTargetRouteIndex < 0 || LastSingleTargetRouteIndex >= Targets.Num())
        {
            LastSingleTargetRouteIndex = 0;
        }
        else
        {
            LastSingleTargetRouteIndex = (LastSingleTargetRouteIndex + 1) % Targets.Num();
        }
        StartIndex = LastSingleTargetRouteIndex;
    }

    for (int32 i = 0; i < NumToRoute; ++i)
    {
        const int32 TargetIndex = bBroadcastToAllNearby ? i : StartIndex;
        ULocalCharacterComponent* Target = Targets.IsValidIndex(TargetIndex) ? Targets[TargetIndex] : nullptr;
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
            TEXT("[AutoChatVoiceInput] Routing transcript to AI \"%s\" at index %d/%d (targetIndex=%d rotateSingle=%d)"),
            *TargetOwnerName,
            i + 1,
            NumToRoute,
            TargetIndex,
            (!bBroadcastToAllNearby && bRotateSingleTargetAcrossNearby) ? 1 : 0);

        Sub->RequestTurn(Target, Text, /*bFromUser*/true);
        ++RoutedCount;
    }

    if (RoutedCount > 0)
    {
        LastTranscriptSubmitWorldSeconds = World->GetTimeSeconds();
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

FString UAutoChatVoiceInputComponent::NormalizeTranscriptForRouting(const FString& InText) const
{
    FString S = InText;
    S.TrimStartAndEndInline();
    if (S.IsEmpty())
    {
        return FString();
    }

    S.ReplaceInline(TEXT("\r"), TEXT(" "));
    S.ReplaceInline(TEXT("\n"), TEXT(" "));
    S.ReplaceInline(TEXT("\t"), TEXT(" "));
    while (S.Contains(TEXT("  ")))
    {
        S.ReplaceInline(TEXT("  "), TEXT(" "));
    }
    S.TrimStartAndEndInline();

    if (S.Len() < MinAcceptedTranscriptChars)
    {
        return FString();
    }

    if (S.Len() > MaxAcceptedTranscriptChars)
    {
        S = S.Left(MaxAcceptedTranscriptChars);
        S.TrimEndInline();
    }

    int32 AlphaCount = 0;
    int32 DigitCount = 0;
    for (int32 i = 0; i < S.Len(); ++i)
    {
        const TCHAR C = S[i];
        if (FChar::IsAlpha(C))
        {
            AlphaCount++;
        }
        else if (FChar::IsDigit(C))
        {
            DigitCount++;
        }
    }

    TArray<FString> Tokens;
    S.ParseIntoArrayWS(Tokens);
    int32 NumNumericOnly = 0;
    int32 NumSingleChar = 0;
    for (const FString& Token : Tokens)
    {
        if (Token.Len() == 1)
        {
            NumSingleChar++;
        }

        bool bAllDigits = !Token.IsEmpty();
        for (int32 i = 0; i < Token.Len(); ++i)
        {
            if (!FChar::IsDigit(Token[i]))
            {
                bAllDigits = false;
                break;
            }
        }
        if (bAllDigits)
        {
            NumNumericOnly++;
        }
    }

    // Reject number/noise-heavy transcripts (for example: "8 8 8 8 8 ...").
    if (Tokens.Num() >= 8)
    {
        const float NumericRatio = (float)NumNumericOnly / (float)Tokens.Num();
        const float SingleCharRatio = (float)NumSingleChar / (float)Tokens.Num();
        if (NumericRatio >= 0.55f || SingleCharRatio >= 0.70f)
        {
            return FString();
        }
    }

    if (AlphaCount < 8 && DigitCount > (AlphaCount * 2))
    {
        return FString();
    }

    return S;
}

bool UAutoChatVoiceInputComponent::IsTranscriptLikelyRepetitionSpam(const FString& InText, FString& OutReason) const
{
    OutReason.Reset();
    if (InText.IsEmpty())
    {
        return false;
    }

    TArray<FString> Sentences;
    {
        FString Current;
        Current.Reserve(InText.Len());
        for (int32 i = 0; i < InText.Len(); ++i)
        {
            const TCHAR C = InText[i];
            const bool bTerminator = (C == TEXT('.')) || (C == TEXT('!')) || (C == TEXT('?')) || (C == TEXT('\n'));
            if (bTerminator)
            {
                FString Part = Current.TrimStartAndEnd();
                if (!Part.IsEmpty())
                {
                    Sentences.Add(Part.ToLower());
                }
                Current.Reset();
                continue;
            }
            Current.AppendChar(C);
        }

        FString Tail = Current.TrimStartAndEnd();
        if (!Tail.IsEmpty())
        {
            Sentences.Add(Tail.ToLower());
        }
    }

    if (Sentences.Num() < RepetitionSentenceThreshold)
    {
        return false;
    }

    TMap<FString, int32> Counts;
    int32 MaxCount = 0;
    FString Dominant;
    for (const FString& Sentence : Sentences)
    {
        int32& C = Counts.FindOrAdd(Sentence);
        C++;
        if (C > MaxCount)
        {
            MaxCount = C;
            Dominant = Sentence;
        }
    }

    if (Sentences.Num() <= 0)
    {
        return false;
    }

    const float DominantRatio = (float)MaxCount / (float)Sentences.Num();
    if (DominantRatio >= MaxDominantSentenceRatio && Dominant.Len() >= 4)
    {
        OutReason = FString::Printf(
            TEXT("dominantSentenceRatio=%.2f (%d/%d) dominant=\"%s\""),
            DominantRatio,
            MaxCount,
            Sentences.Num(),
            *Dominant.Left(80));
        return true;
    }

    return false;
}

bool UAutoChatVoiceInputComponent::IsTranscriptLikelyNpcEcho(
    const FString& InText,
    const ULocalTalkConversationSubsystem* Sub,
    const TArray<ULocalCharacterComponent*>& CandidateTargets,
    FString& OutReason) const
{
    OutReason.Reset();
    if (!Sub || CandidateTargets.Num() == 0 || InText.IsEmpty())
    {
        return false;
    }

    TArray<FString> InputTokens;
    LocalVoiceTokenize(InText, InputTokens);
    if (InputTokens.Num() < 4)
    {
        return false;
    }

    TArray<FString> RecentNpcLines;
    TSet<FString> SeenLower;
    const int32 PerTargetLimit = FMath::Clamp(NpcEchoRecentNpcLines, 1, 12);
    for (ULocalCharacterComponent* Target : CandidateTargets)
    {
        if (!Target)
        {
            continue;
        }

        TArray<FLocalTalkMessage> History = const_cast<ULocalTalkConversationSubsystem*>(Sub)->GetContextHistory(Target);
        int32 AddedForTarget = 0;
        for (int32 i = History.Num() - 1; i >= 0 && AddedForTarget < PerTargetLimit; --i)
        {
            const FLocalTalkMessage& M = History[i];
            if (M.bFromUser)
            {
                continue;
            }

            FString Line = M.Content;
            Line.ReplaceInline(TEXT("\r"), TEXT(" "));
            Line.ReplaceInline(TEXT("\n"), TEXT(" "));
            Line.TrimStartAndEndInline();
            if (Line.Len() < MinAcceptedTranscriptChars)
            {
                continue;
            }

            const FString Lower = Line.ToLower();
            if (SeenLower.Contains(Lower))
            {
                continue;
            }

            SeenLower.Add(Lower);
            RecentNpcLines.Add(Line);
            AddedForTarget++;
        }
    }

    if (RecentNpcLines.Num() == 0)
    {
        return false;
    }

    float BestScore = 0.0f;
    FString BestLine;
    for (const FString& NpcLine : RecentNpcLines)
    {
        TArray<FString> NpcTokens;
        LocalVoiceTokenize(NpcLine, NpcTokens);
        if (NpcTokens.Num() < 3)
        {
            continue;
        }

        const float Score = LocalVoiceTokenOverlapScore(InputTokens, NpcTokens);
        if (Score > BestScore)
        {
            BestScore = Score;
            BestLine = NpcLine;
        }
    }

    if (BestScore >= FMath::Clamp(NpcEchoSimilarityThreshold, 0.50f, 1.0f))
    {
        OutReason = FString::Printf(
            TEXT("similarity=%.2f threshold=%.2f npcLine=\"%s\""),
            BestScore,
            NpcEchoSimilarityThreshold,
            *BestLine.Left(90));
        return true;
    }

    return false;
}

bool UAutoChatVoiceInputComponent::IsTranscriptLikelyLowQuality(const FString& InText, FString& OutReason) const
{
    OutReason.Reset();
    if (InText.IsEmpty())
    {
        return false;
    }

    TArray<FString> Tokens;
    LocalVoiceTokenize(InText, Tokens);
    if (Tokens.Num() < 8)
    {
        return false;
    }

    int32 DominantTokenCount = 0;
    const float UniqueRatio = LocalVoiceUniqueTokenRatio(Tokens, DominantTokenCount);
    const float DominantRatio = (float)DominantTokenCount / (float)Tokens.Num();

    if (UniqueRatio < FMath::Clamp(MinTranscriptUniqueTokenRatio, 0.20f, 1.0f) && DominantRatio >= 0.30f)
    {
        OutReason = FString::Printf(
            TEXT("uniqueRatio=%.2f dominantTokenRatio=%.2f tokens=%d"),
            UniqueRatio,
            DominantRatio,
            Tokens.Num());
        return true;
    }

    if (Tokens.Num() >= 10)
    {
        TMap<FString, int32> TrigramCounts;
        int32 MaxTrigramCount = 0;
        FString DominantTrigram;
        for (int32 i = 0; i + 2 < Tokens.Num(); ++i)
        {
            const FString Tri = Tokens[i] + TEXT(" ") + Tokens[i + 1] + TEXT(" ") + Tokens[i + 2];
            int32& C = TrigramCounts.FindOrAdd(Tri);
            C++;
            if (C > MaxTrigramCount)
            {
                MaxTrigramCount = C;
                DominantTrigram = Tri;
            }
        }

        if (MaxTrigramCount >= 2 && UniqueRatio < 0.70f)
        {
            OutReason = FString::Printf(
                TEXT("repeatedTrigram=%d trigram=\"%s\" uniqueRatio=%.2f"),
                MaxTrigramCount,
                *DominantTrigram.Left(60),
                UniqueRatio);
            return true;
        }
    }

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
        bool bFound = false;
        for (int32 Index = 0; Index < Devices.Num(); ++Index)
        {
            if (Devices[Index].DeviceName.Equals(MicInputDeviceName, ESearchCase::IgnoreCase))
            {
                TargetDeviceIndex = Index;
                TargetDeviceName = Devices[Index].DeviceName;
                bFound = true;
                break;
            }
        }

        if (!bFound)
        {
            UE_LOG(LogAutoChatVoice, Warning,
                TEXT("[AutoChatVoiceInput] Requested named mic \"%s\" not found; falling back to default system input."),
                *MicInputDeviceName);
        }
    }
    else
    {
        TargetDeviceIndex = INDEX_NONE;
        TargetDeviceName = TEXT("Default (System)");
    }

    Audio::FAudioCaptureDeviceParams Params;
    Params.DeviceIndex = TargetDeviceIndex;

    const int32 FramesPerBuffer = 1024;

    if (!MicCapture.OpenAudioCaptureStream(
        Params,
        [this](const void* InAudio, int32 NumFrames, int32 NumChannels, int32 SampleRate, double StreamTime, bool bOverflow)
        {
            const float* FloatAudio = static_cast<const float*>(InAudio);
            OnAudioCapture(FloatAudio, NumFrames, NumChannels, SampleRate, StreamTime, bOverflow);
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
    NextMicActivityLogTimeSeconds = 0.0;
    bAutoSpeechSegmentActive = false;
    bAutoSpeechSegmentBargeInTriggered = false;
    bAutoSpeechSegmentHadAudibleAI = false;
    AutoSpeechStartGateFrames = 0;
    AutoSpeechTotalFrames = 0;
    AutoSpeechActiveFrames = 0;
    AutoSpeechLoudFrames = 0;
    AutoSpeechPcm16.Reset();
    AdaptiveNoiseFloorRms = 0.0f;
    AutoSpeechSampleRate = 0;
    AutoSpeechNumChannels = 0;
    AutoSpeechStartWorldSeconds = 0.0;
    AutoSpeechLastActiveWorldSeconds = 0.0;

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
    bAutoSpeechSegmentActive = false;
    bAutoSpeechSegmentBargeInTriggered = false;
    bAutoSpeechSegmentHadAudibleAI = false;
    AutoSpeechStartGateFrames = 0;
    AutoSpeechTotalFrames = 0;
    AutoSpeechActiveFrames = 0;
    AutoSpeechLoudFrames = 0;
    AutoSpeechPcm16.Reset();
    AdaptiveNoiseFloorRms = 0.0f;
    AutoSpeechSampleRate = 0;
    AutoSpeechNumChannels = 0;
    AutoSpeechStartWorldSeconds = 0.0;
    AutoSpeechLastActiveWorldSeconds = 0.0;

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

void UAutoChatVoiceInputComponent::OnAudioCapture(const float* AudioData, int32 NumFrames, int32 NumChannels, int32 SampleRate, double StreamTime, bool bOverflow)
{
    (void)StreamTime;
    (void)bOverflow;

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

    // Always feed segment logic, including silence frames, so segment end timing
    // is based on real elapsed silence rather than delayed until next noise spike.
    if (bAlwaysOnAutoTranscribe)
    {
        ProcessAlwaysOnAutoTranscribe(AudioData, NumFrames, NumChannels, SampleRate, Rms);
    }

    // Simple threshold to decide whether there's "voice-like" activity for logs/UI.
    static constexpr float ActivityThreshold = 0.01f;
    if (Rms < ActivityThreshold)
    {
        return;
    }

    if (const UWorld* W = GetWorld())
    {
        const double Now = (double)W->GetTimeSeconds();
        if (Now >= NextMicActivityLogTimeSeconds)
        {
            NextMicActivityLogTimeSeconds = Now + 1.0;
            UE_LOG(LogAutoChatVoice, Log, TEXT("[AutoChatVoiceInput] Mic activity detected (rms=%.4f samples=%d)."), Rms, NumSamples);
        }
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

void UAutoChatVoiceInputComponent::ProcessAlwaysOnAutoTranscribe(const float* AudioData, int32 NumFrames, int32 NumChannels, int32 SampleRate, float Rms)
{
    if (!bMicCaptureActive || !AudioData || NumFrames <= 0 || NumChannels <= 0 || SampleRate <= 0)
    {
        return;
    }

    if (bWhisperCaptureInProgress || bAutoTranscribeRequestInFlight)
    {
        return;
    }

    const UWorld* W = GetWorld();
    if (!W)
    {
        return;
    }
    const double Now = (double)W->GetTimeSeconds();
    const int32 NumSamples = NumFrames * NumChannels;
    if (NumSamples <= 0)
    {
        return;
    }

    const float InputRms = FMath::Clamp(Rms, 0.0f, 1.0f);

    if (bUseAdaptiveNoiseFloor)
    {
        const float MinFloor = FMath::Clamp(AdaptiveNoiseFloorMinRms, 0.001f, 0.20f);
        const float MaxFloor = FMath::Clamp(AdaptiveNoiseFloorMaxRms, MinFloor + 0.001f, 0.30f);
        const float Target = FMath::Clamp(InputRms, MinFloor, MaxFloor);
        const float Alpha = FMath::Clamp(AdaptiveNoiseFloorSmoothing, 0.001f, 1.0f);

        if (AdaptiveNoiseFloorRms <= 0.0f)
        {
            AdaptiveNoiseFloorRms = Target;
        }
        else
        {
            const bool bLikelyNoiseSample = !bAutoSpeechSegmentActive || (InputRms <= AutoTranscribeStartRmsThreshold);
            const float UseAlpha = bLikelyNoiseSample ? Alpha : (Alpha * 0.25f);
            AdaptiveNoiseFloorRms = FMath::Lerp(AdaptiveNoiseFloorRms, Target, UseAlpha);
            AdaptiveNoiseFloorRms = FMath::Clamp(AdaptiveNoiseFloorRms, MinFloor, MaxFloor);
        }
    }
    else
    {
        AdaptiveNoiseFloorRms = 0.0f;
    }

    float EffectiveStartRms = bUseAdaptiveNoiseFloor
        ? FMath::Max(AutoTranscribeStartRmsThreshold, AdaptiveNoiseFloorRms * FMath::Max(1.0f, AdaptiveStartThresholdMultiplier) + 0.002f)
        : AutoTranscribeStartRmsThreshold;
    float EffectiveContinueRms = bUseAdaptiveNoiseFloor
        ? FMath::Max(AutoTranscribeContinueRmsThreshold, AdaptiveNoiseFloorRms * FMath::Max(1.0f, AdaptiveContinueThresholdMultiplier) + 0.001f)
        : AutoTranscribeContinueRmsThreshold;
    if (MaxEffectiveStartRmsThreshold > 0.0f)
    {
        EffectiveStartRms = FMath::Min(EffectiveStartRms, MaxEffectiveStartRmsThreshold);
    }
    if (MaxEffectiveContinueRmsThreshold > 0.0f)
    {
        EffectiveContinueRms = FMath::Min(EffectiveContinueRms, MaxEffectiveContinueRmsThreshold);
    }
    EffectiveContinueRms = FMath::Min(EffectiveContinueRms, EffectiveStartRms);
    const float LowGainStartRms = FMath::Max(
        0.009f,
        FMath::Max(
            AutoTranscribeContinueRmsThreshold * 0.55f,
            AutoTranscribeStartRmsThreshold * 0.35f));
    const float AdaptiveFallbackStartRms = bUseAdaptiveNoiseFloor
        ? (AdaptiveNoiseFloorRms * 1.20f + 0.0005f)
        : 0.0f;
    const float FallbackStartRms = FMath::Clamp(
        FMath::Min(EffectiveStartRms, FMath::Max(LowGainStartRms, AdaptiveFallbackStartRms)),
        0.0085f,
        EffectiveStartRms);
    const float FallbackStartHoldSeconds = FMath::Max(0.28f, AutoTranscribeStartHoldSeconds * 2.5f);
    const float LoudRmsBase = bUseAdaptiveNoiseFloor
        ? FMath::Max(BargeInMinRmsThreshold, AdaptiveNoiseFloorRms * FMath::Max(1.0f, AdaptiveBargeInThresholdMultiplier) + 0.004f)
        : BargeInMinRmsThreshold;
    const float LowGainLoudRms = FMath::Max(
        0.018f,
        FMath::Max(FallbackStartRms * 1.25f, EffectiveContinueRms * 1.10f));
    const float EffectiveLoudRms = FMath::Min(LoudRmsBase, LowGainLoudRms);
    const float EffectiveVeryLoudRms = FMath::Max(
        EffectiveLoudRms * 1.8f,
        FMath::Max(0.035f, FallbackStartRms * 2.5f));
    const bool bNearbyAIAudibleNow = HasNearbyAudibleAISpeech();

    auto AppendChunkPcm16 = [&]()
    {
        const int32 Base = AutoSpeechPcm16.Num();
        AutoSpeechPcm16.AddUninitialized(NumSamples);
        int16* Dest = AutoSpeechPcm16.GetData() + Base;
        for (int32 i = 0; i < NumSamples; ++i)
        {
            const float Clamped = FMath::Clamp(AudioData[i], -1.0f, 1.0f);
            Dest[i] = static_cast<int16>(FMath::RoundToInt(Clamped * 32767.0f));
        }
    };

    auto TryTriggerBargeIn = [&](const TCHAR* Phase)
    {
        if (bAutoSpeechSegmentBargeInTriggered || !bInterruptNearbyAIOnVoiceCaptureStart)
        {
            return;
        }
        if (bBargeInRequiresAudibleAIVoice && !bNearbyAIAudibleNow)
        {
            return;
        }

        const double ActiveSeconds = (AutoSpeechSampleRate > 0)
            ? (double)AutoSpeechActiveFrames / (double)AutoSpeechSampleRate
            : 0.0;
        const double LoudSeconds = (AutoSpeechSampleRate > 0)
            ? (double)AutoSpeechLoudFrames / (double)AutoSpeechSampleRate
            : 0.0;
        const double ActiveRatio = (AutoSpeechTotalFrames > 0)
            ? (double)AutoSpeechActiveFrames / (double)AutoSpeechTotalFrames
            : 0.0;

        const bool bForceSustainedInterrupt =
            (BargeInForceInterruptActiveSeconds > 0.0f) &&
            (ActiveSeconds >= (double)BargeInForceInterruptActiveSeconds) &&
            (ActiveRatio >= (double)AutoTranscribeMinActiveRatio);

        const double SinceSubmit = Now - LastTranscriptSubmitWorldSeconds;
        if (SinceSubmit < (double)PostTranscriptBargeInGraceSeconds && !bForceSustainedInterrupt)
        {
            return;
        }

        const bool bSustainedVoiceForBargeIn =
            ActiveSeconds >= (double)BargeInMinActiveSpeechSeconds &&
            LoudSeconds >= (double)BargeInMinLoudSeconds;
        const bool bVeryLoudOverride =
            InputRms >= EffectiveVeryLoudRms &&
            ActiveSeconds >= (double)BargeInMinActiveSpeechSeconds;

        if (!bSustainedVoiceForBargeIn && !bVeryLoudOverride && !bForceSustainedInterrupt)
        {
            return;
        }

        bAutoSpeechSegmentBargeInTriggered = true;
        UE_LOG(LogAutoChatVoice, Log,
            TEXT("[AutoChatVoiceInput] AlwaysOn barge-in trigger %s (rms=%.4f loudThr=%.4f active=%.2fs loud=%.2fs ratio=%.2f sustained=%d veryLoud=%d forced=%d)."),
            Phase ? Phase : TEXT("UNKNOWN"),
            InputRms,
            EffectiveLoudRms,
            ActiveSeconds,
            LoudSeconds,
            ActiveRatio,
            bSustainedVoiceForBargeIn ? 1 : 0,
            bVeryLoudOverride ? 1 : 0,
            bForceSustainedInterrupt ? 1 : 0);
        AsyncTask(ENamedThreads::GameThread, [WeakThis = TWeakObjectPtr<UAutoChatVoiceInputComponent>(this)]()
        {
            if (UAutoChatVoiceInputComponent* Self = WeakThis.Get())
            {
                Self->InterruptNearbyAIsForPlayerSpeech();
            }
        });
    };

    if (!bAutoSpeechSegmentActive)
    {
        const bool bPassPrimaryStart = (InputRms >= EffectiveStartRms);
        const bool bPassFallbackStart = !bPassPrimaryStart && (InputRms >= FallbackStartRms);
        if (bPassPrimaryStart || bPassFallbackStart)
        {
            AutoSpeechStartGateFrames += NumFrames;
            const double StartGateSeconds = (SampleRate > 0)
                ? ((double)AutoSpeechStartGateFrames / (double)SampleRate)
                : 0.0;
            const float RequiredHoldSeconds = bPassPrimaryStart
                ? FMath::Max(0.01f, AutoTranscribeStartHoldSeconds)
                : FallbackStartHoldSeconds;
            if (StartGateSeconds < (double)RequiredHoldSeconds)
            {
                return;
            }

            AutoSpeechStartGateFrames = 0;
            bAutoSpeechSegmentActive = true;
            bAutoSpeechSegmentBargeInTriggered = false;
            bAutoSpeechSegmentHadAudibleAI = bNearbyAIAudibleNow;
            AutoSpeechStartWorldSeconds = Now;
            AutoSpeechLastActiveWorldSeconds = Now;
            LastActiveSegmentPriorityApplyWorldSeconds = -1.0;
            AutoSpeechSampleRate = SampleRate;
            AutoSpeechNumChannels = NumChannels;
            AutoSpeechTotalFrames = 0;
            AutoSpeechActiveFrames = 0;
            AutoSpeechLoudFrames = 0;
            AutoSpeechPcm16.Reset();
            AutoSpeechPcm16.Reserve(NumSamples * 8);
            AppendChunkPcm16();
            AutoSpeechTotalFrames += NumFrames;
            if (InputRms >= EffectiveContinueRms)
            {
                AutoSpeechActiveFrames += NumFrames;
            }
            if (InputRms >= EffectiveLoudRms)
            {
                AutoSpeechLoudFrames += NumFrames;
            }

            UE_LOG(LogAutoChatVoice, Log,
                TEXT("[AutoChatVoiceInput] AlwaysOn segment START (rms=%.4f startMode=%s effStart=%.4f fbStart=%.4f effCont=%.4f effLoud=%.4f noiseFloor=%.4f hold=%.2fs sr=%d ch=%d)."),
                InputRms,
                bPassPrimaryStart ? TEXT("primary") : TEXT("fallback"),
                EffectiveStartRms,
                FallbackStartRms,
                EffectiveContinueRms,
                EffectiveLoudRms,
                AdaptiveNoiseFloorRms,
                RequiredHoldSeconds,
                SampleRate,
                NumChannels);

            if (bFenceDuringAlwaysOnSegment)
            {
                AsyncTask(ENamedThreads::GameThread, [WeakThis = TWeakObjectPtr<UAutoChatVoiceInputComponent>(this)]()
                {
                    if (UAutoChatVoiceInputComponent* Self = WeakThis.Get())
                    {
                        Self->BeginPlayerSpeechFence(Self->PlayerSpeechFenceMaxSeconds, TEXT("always_on_segment_start"));
                    }
                });
            }

            TryTriggerBargeIn(TEXT("at segment START"));
        }
        else
        {
            AutoSpeechStartGateFrames = 0;
        }
        return;
    }

    // Segment active.
    if (NumChannels != AutoSpeechNumChannels || SampleRate != AutoSpeechSampleRate)
    {
        UE_LOG(LogAutoChatVoice, Warning,
            TEXT("[AutoChatVoiceInput] AlwaysOn segment reset due to format change (old=%dHz/%dch new=%dHz/%dch)."),
            AutoSpeechSampleRate,
            AutoSpeechNumChannels,
            SampleRate,
            NumChannels);
        bAutoSpeechSegmentActive = false;
        bAutoSpeechSegmentBargeInTriggered = false;
        bAutoSpeechSegmentHadAudibleAI = false;
        LastActiveSegmentPriorityApplyWorldSeconds = -1.0;
        AutoSpeechStartGateFrames = 0;
        AutoSpeechTotalFrames = 0;
        AutoSpeechActiveFrames = 0;
        AutoSpeechLoudFrames = 0;
        AutoSpeechPcm16.Reset();
        AutoSpeechSampleRate = 0;
        AutoSpeechNumChannels = 0;
        return;
    }

    AppendChunkPcm16();
    AutoSpeechTotalFrames += NumFrames;
    if (InputRms >= EffectiveContinueRms)
    {
        AutoSpeechActiveFrames += NumFrames;
        AutoSpeechLastActiveWorldSeconds = Now;
    }
    if (InputRms >= EffectiveLoudRms)
    {
        AutoSpeechLoudFrames += NumFrames;
    }
    if (bNearbyAIAudibleNow)
    {
        bAutoSpeechSegmentHadAudibleAI = true;
    }

    // While a player segment is actively accumulating speech, keep a short
    // priority window refreshed so NPC auto-turns don't race ahead of the
    // in-flight transcript context.
    {
        const double ActiveSpeechSecondsNow = (AutoSpeechSampleRate > 0)
            ? ((double)AutoSpeechActiveFrames / (double)AutoSpeechSampleRate)
            : 0.0;
        const double ActiveRatioNow = (AutoSpeechTotalFrames > 0)
            ? ((double)AutoSpeechActiveFrames / (double)AutoSpeechTotalFrames)
            : 0.0;

        const double MinActiveForPriority = (double)FMath::Max(0.18f, AutoTranscribeMinActiveSpeechSeconds * 0.75f);
        const double MinRatioForPriority = (double)FMath::Max(0.35f, AutoTranscribeMinActiveRatio * 0.90f);
        const bool bLikelyHumanSpeechNow =
            (ActiveSpeechSecondsNow >= MinActiveForPriority) ||
            ((AutoSpeechTotalFrames >= FMath::Max(1, AutoSpeechSampleRate / 2)) && (ActiveRatioNow >= MinRatioForPriority));

        if (ActiveSegmentPriorityHoldSeconds > 0.0f && bLikelyHumanSpeechNow)
        {
            const double RefreshCadence = (double)FMath::Max(0.10f, ActiveSegmentPriorityRefreshSeconds);
            if (LastActiveSegmentPriorityApplyWorldSeconds < 0.0 ||
                (Now - LastActiveSegmentPriorityApplyWorldSeconds) >= RefreshCadence)
            {
                LastActiveSegmentPriorityApplyWorldSeconds = Now;
                AsyncTask(ENamedThreads::GameThread, [WeakThis = TWeakObjectPtr<UAutoChatVoiceInputComponent>(this)]()
                {
                    if (UAutoChatVoiceInputComponent* Self = WeakThis.Get())
                    {
                        Self->ApplyPlayerSpeechPriorityWindow(Self->ActiveSegmentPriorityHoldSeconds, TEXT("always_on_segment_active"));
                    }
                });
            }
        }
    }

    TryTriggerBargeIn(TEXT("DURING segment"));

    const double SegmentSeconds = Now - AutoSpeechStartWorldSeconds;
    const double SilenceSeconds = Now - AutoSpeechLastActiveWorldSeconds;
    const bool bReachedMax = SegmentSeconds >= (double)AutoTranscribeMaxSpeechSeconds;
    const bool bSilenceEnded = (SilenceSeconds >= (double)AutoTranscribeSilenceSeconds) &&
        (SegmentSeconds >= (double)AutoTranscribeMinSpeechSeconds);
    if (!bReachedMax && !bSilenceEnded)
    {
        return;
    }

    TArray<int16> SegmentPcm = MoveTemp(AutoSpeechPcm16);
    const int32 SegmentSampleRate = AutoSpeechSampleRate;
    const int32 SegmentNumChannels = AutoSpeechNumChannels;
    const int32 SegmentTotalFrames = AutoSpeechTotalFrames;
    const int32 SegmentActiveFrames = AutoSpeechActiveFrames;
    const bool bSegmentBargeInTriggered = bAutoSpeechSegmentBargeInTriggered;
    const bool bSegmentHadAudibleAI = bAutoSpeechSegmentHadAudibleAI;
    bAutoSpeechSegmentActive = false;
    bAutoSpeechSegmentBargeInTriggered = false;
    bAutoSpeechSegmentHadAudibleAI = false;
    LastActiveSegmentPriorityApplyWorldSeconds = -1.0;
    AutoSpeechStartGateFrames = 0;
    AutoSpeechSampleRate = 0;
    AutoSpeechNumChannels = 0;
    AutoSpeechTotalFrames = 0;
    AutoSpeechActiveFrames = 0;
    AutoSpeechLoudFrames = 0;
    AutoSpeechStartWorldSeconds = 0.0;
    AutoSpeechLastActiveWorldSeconds = 0.0;

    const double BufferSeconds = (SegmentSampleRate > 0 && SegmentNumChannels > 0)
        ? ((double)SegmentPcm.Num() / (double)(SegmentSampleRate * SegmentNumChannels))
        : 0.0;
    const double ActiveSpeechSeconds = (SegmentSampleRate > 0)
        ? ((double)SegmentActiveFrames / (double)SegmentSampleRate)
        : 0.0;
    const double ActiveRatio = (SegmentTotalFrames > 0)
        ? ((double)SegmentActiveFrames / (double)SegmentTotalFrames)
        : 0.0;

    const bool bLikelyAIBleedNoise = (ActiveSpeechSeconds < 0.90) || (ActiveRatio < 0.55);
    if (bDropAIAudibleSegmentsWithoutBargeIn && bSegmentHadAudibleAI && !bSegmentBargeInTriggered && bLikelyAIBleedNoise)
    {
        UE_LOG(LogAutoChatVoice, Log,
            TEXT("[AutoChatVoiceInput] AlwaysOn segment dropped (ai-audible overlap without barge-in: dur=%.2fs active=%.2fs ratio=%.2f samples=%d)."),
            BufferSeconds,
            ActiveSpeechSeconds,
            ActiveRatio,
            SegmentPcm.Num());
        bAutoTranscribeRequestInFlight = false;
        AsyncTask(ENamedThreads::GameThread, [WeakThis = TWeakObjectPtr<UAutoChatVoiceInputComponent>(this)]()
        {
            if (UAutoChatVoiceInputComponent* Self = WeakThis.Get())
            {
                Self->EndPlayerSpeechFence(TEXT("always_on_segment_ai_overlap_no_barge"));
            }
        });
        return;
    }

    if (BufferSeconds < (double)AutoTranscribeMinSpeechSeconds)
    {
        UE_LOG(LogAutoChatVoice, Log,
            TEXT("[AutoChatVoiceInput] AlwaysOn segment dropped (too short: %.2fs < minSpeech %.2fs, samples=%d)."),
            BufferSeconds,
            AutoTranscribeMinSpeechSeconds,
            SegmentPcm.Num());
        bAutoTranscribeRequestInFlight = false;
        AsyncTask(ENamedThreads::GameThread, [WeakThis = TWeakObjectPtr<UAutoChatVoiceInputComponent>(this)]()
        {
            if (UAutoChatVoiceInputComponent* Self = WeakThis.Get())
            {
                Self->EndPlayerSpeechFence(TEXT("always_on_segment_too_short"));
            }
        });
        return;
    }

    if (ActiveSpeechSeconds < (double)AutoTranscribeMinActiveSpeechSeconds ||
        ActiveRatio < (double)AutoTranscribeMinActiveRatio)
    {
        UE_LOG(LogAutoChatVoice, Log,
            TEXT("[AutoChatVoiceInput] AlwaysOn segment dropped (likely non-voice: active=%.2fs ratio=%.2f minActive=%.2fs minRatio=%.2f dur=%.2fs samples=%d)."),
            ActiveSpeechSeconds,
            ActiveRatio,
            AutoTranscribeMinActiveSpeechSeconds,
            AutoTranscribeMinActiveRatio,
            BufferSeconds,
            SegmentPcm.Num());
        bAutoTranscribeRequestInFlight = false;
        AsyncTask(ENamedThreads::GameThread, [WeakThis = TWeakObjectPtr<UAutoChatVoiceInputComponent>(this)]()
        {
            if (UAutoChatVoiceInputComponent* Self = WeakThis.Get())
            {
                Self->EndPlayerSpeechFence(TEXT("always_on_segment_non_voice"));
            }
        });
        return;
    }

    UE_LOG(LogAutoChatVoice, Log,
        TEXT("[AutoChatVoiceInput] AlwaysOn segment END (reason=%s duration=%.2fs samples=%d) -> sending to Whisper."),
        bReachedMax ? TEXT("max_duration") : TEXT("silence"),
        BufferSeconds,
        SegmentPcm.Num());

    bAutoTranscribeRequestInFlight = true;
    AsyncTask(ENamedThreads::GameThread, [WeakThis = TWeakObjectPtr<UAutoChatVoiceInputComponent>(this), Segment = MoveTemp(SegmentPcm), SegmentSampleRate, SegmentNumChannels]()
    {
        UAutoChatVoiceInputComponent* Self = WeakThis.Get();
        if (!Self)
        {
            return;
        }

        Self->BeginPlayerSpeechFence(Self->PlayerSpeechFenceMaxSeconds, TEXT("always_on_transcribe_inflight"));
        Self->ApplyPlayerSpeechPriorityWindow(Self->PostSegmentPriorityHoldSeconds, TEXT("always_on_segment_end"));

        if (!Self->EnsureWhisperBridge() || !Self->WhisperBridgeComponent)
        {
            Self->bAutoTranscribeRequestInFlight = false;
            Self->OnVoiceError.Broadcast(TEXT("Whisper bridge unavailable for always-on transcription."));
            UE_LOG(LogAutoChatVoice, Warning, TEXT("[AutoChatVoiceInput] AlwaysOn failed: Whisper bridge unavailable."));
            Self->EndPlayerSpeechFence(TEXT("always_on_no_bridge"));
            return;
        }

        Self->WhisperBridgeComponent->bUseProjectSettingsMicInput = false;
        Self->WhisperBridgeComponent->MicInputDeviceMode = Self->MicInputDeviceMode;
        Self->WhisperBridgeComponent->MicInputDeviceName = Self->MicInputDeviceName;

        const bool bQueued = Self->WhisperBridgeComponent->TranscribePcm16Buffer(
            Segment,
            SegmentSampleRate,
            SegmentNumChannels,
            /*bSendToNearestAI*/ false,
            Self->MaxInteractionRange);

        if (!bQueued)
        {
            Self->bAutoTranscribeRequestInFlight = false;
            Self->OnVoiceError.Broadcast(TEXT("Always-on transcription request failed to queue."));
            UE_LOG(LogAutoChatVoice, Warning, TEXT("[AutoChatVoiceInput] AlwaysOn failed: could not queue transcription."));
            Self->EndPlayerSpeechFence(TEXT("always_on_queue_failed"));
            return;
        }

        const double SegDur = (SegmentSampleRate > 0 && SegmentNumChannels > 0)
            ? ((double)Segment.Num() / (double)(SegmentSampleRate * SegmentNumChannels))
            : 0.0;
        UE_LOG(LogAutoChatVoice, Log,
            TEXT("[AutoChatVoiceInput] AlwaysOn sent to Whisper: duration=%.2fs samples=%d sr=%d ch=%d."),
            SegDur, Segment.Num(), SegmentSampleRate, SegmentNumChannels);
    });
}

bool UAutoChatVoiceInputComponent::EnsureWhisperBridge()
{
    if (WhisperBridgeComponent)
    {
        return true;
    }

    AActor* Owner = GetOwner();
    if (!Owner)
    {
        return false;
    }

    WhisperBridgeComponent = Owner->FindComponentByClass<ULocalPlayerInteractionComponent>();
    if (!WhisperBridgeComponent)
    {
        WhisperBridgeComponent = NewObject<ULocalPlayerInteractionComponent>(Owner, TEXT("AutoChatWhisperBridge"));
        if (!WhisperBridgeComponent)
        {
            return false;
        }
        WhisperBridgeComponent->RegisterComponent();
    }

    WhisperBridgeComponent->OnWhisperTranscription.RemoveDynamic(this, &UAutoChatVoiceInputComponent::HandleWhisperTranscription);
    WhisperBridgeComponent->OnWhisperError.RemoveDynamic(this, &UAutoChatVoiceInputComponent::HandleWhisperError);
    WhisperBridgeComponent->OnWhisperTranscription.AddDynamic(this, &UAutoChatVoiceInputComponent::HandleWhisperTranscription);
    WhisperBridgeComponent->OnWhisperError.AddDynamic(this, &UAutoChatVoiceInputComponent::HandleWhisperError);
    return true;
}

void UAutoChatVoiceInputComponent::HandleWhisperTranscription(const FString& Transcript)
{
    bWhisperCaptureInProgress = false;
    bAutoTranscribeRequestInFlight = false;
    StartMicCapture();

    const FString Text = Transcript.TrimStartAndEnd();
    if (Text.IsEmpty())
    {
        UE_LOG(LogAutoChatVoice, Log, TEXT("[AutoChatVoiceInput] Whisper transcript empty; treating as silence (no-op)."));
        EndPlayerSpeechFence(TEXT("whisper_empty"));
        return;
    }

    UE_LOG(LogAutoChatVoice, Log,
        TEXT("[AutoChatVoiceInput] Whisper transcript received (%d chars): \"%s\""),
        Text.Len(), *Text);
    const bool bSubmitted = SubmitRecognizedSpeech(Text);
    UE_LOG(LogAutoChatVoice, Log, TEXT("[AutoChatVoiceInput] Transcript routing result: %s"), bSubmitted ? TEXT("success") : TEXT("failed"));
    EndPlayerSpeechFence(bSubmitted ? TEXT("whisper_submitted") : TEXT("whisper_submit_failed"));
}

void UAutoChatVoiceInputComponent::HandleWhisperError(const FString& Error)
{
    bWhisperCaptureInProgress = false;
    bAutoTranscribeRequestInFlight = false;
    StartMicCapture();

    OnVoiceError.Broadcast(Error);
    UE_LOG(LogAutoChatVoice, Warning, TEXT("[AutoChatVoiceInput] Whisper error: %s"), *Error);
    EndPlayerSpeechFence(TEXT("whisper_error"));
}
