// Automation tests for game-side player voice input routing.
#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "AutoChatVoiceInputComponent.h"
#include "LocalTalkConversationSubsystem.h"
#include "LocalCharacterComponent.h"
#include "Algo/Count.h"
#if WITH_EDITOR
#include "Editor.h"
#endif

namespace
{
UWorld* GetAutomationWorld()
{
#if WITH_EDITOR
    if (GEditor)
    {
        return GEditor->GetEditorWorldContext().World();
    }
#endif
    return nullptr;
}

bool SpawnTalker(
    FAutomationTestBase& Test,
    UWorld* World,
    ULocalTalkConversationSubsystem* Sub,
    const FVector& Location,
    const FString& SpeakerName,
    AActor*& OutActor,
    ULocalCharacterComponent*& OutComponent)
{
    OutActor = nullptr;
    OutComponent = nullptr;

    if (!World || !Sub)
    {
        Test.AddError(TEXT("SpawnTalker called with invalid world/subsystem."));
        return false;
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    OutActor = World->SpawnActor<AActor>(AActor::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
    if (!OutActor)
    {
        Test.AddError(TEXT("Failed to spawn actor for talker."));
        return false;
    }

    OutComponent = NewObject<ULocalCharacterComponent>(OutActor);
    if (!OutComponent)
    {
        Test.AddError(TEXT("Failed to create LocalCharacterComponent."));
        OutActor->Destroy();
        OutActor = nullptr;
        return false;
    }

    if (!OutActor->GetRootComponent())
    {
        USceneComponent* Root = NewObject<USceneComponent>(OutActor, TEXT("TestRoot"));
        if (Root)
        {
            Root->RegisterComponent();
            OutActor->SetRootComponent(Root);
        }
    }

    OutActor->SetActorLocation(Location);
    OutComponent->SpeakerName = SpeakerName;
    OutComponent->ConversationRadius = 800.0f;
    OutComponent->VoiceAttenuationRadius = 800.0f;
    OutComponent->RegisterComponent();
    Sub->RegisterTalker(OutComponent);
    return true;
}

void CleanupTalkers(
    ULocalTalkConversationSubsystem* Sub,
    AActor* A1 = nullptr, ULocalCharacterComponent* C1 = nullptr,
    AActor* A2 = nullptr, ULocalCharacterComponent* C2 = nullptr)
{
    if (Sub)
    {
        if (C1) Sub->UnregisterTalker(C1);
        if (C2) Sub->UnregisterTalker(C2);
    }

    if (A1) A1->Destroy();
    if (A2) A2->Destroy();
}

AActor* SpawnRootedActor(UWorld* World, const FVector& Location)
{
    if (!World)
    {
        return nullptr;
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
    if (!Actor)
    {
        return nullptr;
    }

    if (!Actor->GetRootComponent())
    {
        USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("PlayerTestRoot"));
        if (Root)
        {
            Root->RegisterComponent();
            Actor->SetRootComponent(Root);
        }
    }
    Actor->SetActorLocation(Location);
    return Actor;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAutoChatVoiceInputNearestRoutingTest,
    "Project.AutoChat.VoiceInput.NearestAIMicrophoneAndTranscriptRouting",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutoChatVoiceInputNearestRoutingTest::RunTest(const FString& Parameters)
{
    UWorld* World = GetAutomationWorld();
    if (!World)
    {
        AddError(TEXT("Editor world not found."));
        return false;
    }

    ULocalTalkConversationSubsystem* Sub = World->GetSubsystem<ULocalTalkConversationSubsystem>();
    if (!Sub)
    {
        AddError(TEXT("Conversation subsystem not found."));
        return false;
    }

    AActor* AIActor = nullptr;
    ULocalCharacterComponent* AI = nullptr;
    const FVector Base(300000.0f, 100000.0f, 100.0f);
    if (!SpawnTalker(*this, World, Sub, Base + FVector(150.0f, 0.0f, 0.0f), TEXT("Guide"), AIActor, AI))
    {
        CleanupTalkers(Sub, AIActor, AI);
        return false;
    }

    AActor* PlayerActor = SpawnRootedActor(World, Base);
    if (!PlayerActor)
    {
        AddError(TEXT("Failed to spawn player actor."));
        CleanupTalkers(Sub, AIActor, AI);
        return false;
    }

    UAutoChatVoiceInputComponent* VoiceComp = NewObject<UAutoChatVoiceInputComponent>(PlayerActor);
    if (!VoiceComp)
    {
        AddError(TEXT("Failed to create UAutoChatVoiceInputComponent."));
        PlayerActor->Destroy();
        CleanupTalkers(Sub, AIActor, AI);
        return false;
    }
    VoiceComp->RegisterComponent();
    VoiceComp->MaxInteractionRange = 1000.0f;
    VoiceComp->bBroadcastToAllNearby = false;
    VoiceComp->UseNamedMicrophone(TEXT("Mic A"));

    const FString Transcript = TEXT("Let's head toward the canyon and keep spacing.");
    const bool bOk = VoiceComp->SubmitRecognizedSpeech(Transcript);
    TestTrue(TEXT("SubmitRecognizedSpeech should route to nearest AI."), bOk);

    const TArray<FLocalTalkMessage> History = Sub->GetContextHistory(AI);
    TestEqual(TEXT("History should contain one user line."), History.Num(), 1);
    if (History.Num() == 1)
    {
        TestTrue(TEXT("History entry should be marked from user."), History[0].bFromUser);
        TestEqual(TEXT("History entry should match transcript."), History[0].Content, Transcript);
    }

    TestFalse(TEXT("Target AI should use explicit mic settings."), AI->bUseProjectSettingsMicInput);
    TestEqual(TEXT("Target AI mic mode should be NamedDevice."), AI->MicInputDeviceMode, ELocalTalkMicInputDeviceMode::NamedDevice);
    TestEqual(TEXT("Target AI mic name should match selected mic."), AI->MicInputDeviceName, FString(TEXT("Mic A")));

    PlayerActor->Destroy();
    CleanupTalkers(Sub, AIActor, AI);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAutoChatVoiceInputBroadcastRoutingTest,
    "Project.AutoChat.VoiceInput.BroadcastToAllNearby",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutoChatVoiceInputBroadcastRoutingTest::RunTest(const FString& Parameters)
{
    UWorld* World = GetAutomationWorld();
    if (!World)
    {
        AddError(TEXT("Editor world not found."));
        return false;
    }

    ULocalTalkConversationSubsystem* Sub = World->GetSubsystem<ULocalTalkConversationSubsystem>();
    if (!Sub)
    {
        AddError(TEXT("Conversation subsystem not found."));
        return false;
    }

    AActor* A1 = nullptr;
    AActor* A2 = nullptr;
    ULocalCharacterComponent* C1 = nullptr;
    ULocalCharacterComponent* C2 = nullptr;

    const FVector Base(450000.0f, 120000.0f, 100.0f);
    if (!SpawnTalker(*this, World, Sub, Base + FVector(120.0f, 0.0f, 0.0f), TEXT("A1"), A1, C1) ||
        !SpawnTalker(*this, World, Sub, Base + FVector(220.0f, 0.0f, 0.0f), TEXT("A2"), A2, C2))
    {
        CleanupTalkers(Sub, A1, C1, A2, C2);
        return false;
    }

    AActor* PlayerActor = SpawnRootedActor(World, Base);
    if (!PlayerActor)
    {
        AddError(TEXT("Failed to spawn player actor."));
        CleanupTalkers(Sub, A1, C1, A2, C2);
        return false;
    }

    UAutoChatVoiceInputComponent* VoiceComp = NewObject<UAutoChatVoiceInputComponent>(PlayerActor);
    VoiceComp->RegisterComponent();
    VoiceComp->MaxInteractionRange = 1200.0f;
    VoiceComp->bBroadcastToAllNearby = true;

    const FString Transcript = TEXT("Everyone listen up, regroup by the bridge.");
    const bool bOk = VoiceComp->SubmitRecognizedSpeech(Transcript);
    TestTrue(TEXT("Broadcast mode should route to all nearby AIs."), bOk);

    const TArray<FLocalTalkMessage> H1 = Sub->GetContextHistory(C1);
    const TArray<FLocalTalkMessage> H2 = Sub->GetContextHistory(C2);
    TestTrue(TEXT("History for C1 should include a user line."), H1.Num() >= 1);
    TestTrue(TEXT("History for C2 should include a user line."), H2.Num() >= 1);

    PlayerActor->Destroy();
    CleanupTalkers(Sub, A1, C1, A2, C2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAutoChatVoiceInputOutOfRangeTest,
    "Project.AutoChat.VoiceInput.NoNearbyAIsFails",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutoChatVoiceInputOutOfRangeTest::RunTest(const FString& Parameters)
{
    UWorld* World = GetAutomationWorld();
    if (!World)
    {
        AddError(TEXT("Editor world not found."));
        return false;
    }

    ULocalTalkConversationSubsystem* Sub = World->GetSubsystem<ULocalTalkConversationSubsystem>();
    if (!Sub)
    {
        AddError(TEXT("Conversation subsystem not found."));
        return false;
    }

    AActor* AIActor = nullptr;
    ULocalCharacterComponent* AI = nullptr;
    const FVector Base(600000.0f, 150000.0f, 100.0f);
    if (!SpawnTalker(*this, World, Sub, Base + FVector(5000.0f, 0.0f, 0.0f), TEXT("FarAI"), AIActor, AI))
    {
        CleanupTalkers(Sub, AIActor, AI);
        return false;
    }

    AActor* PlayerActor = SpawnRootedActor(World, Base);
    if (!PlayerActor)
    {
        AddError(TEXT("Failed to spawn player actor."));
        CleanupTalkers(Sub, AIActor, AI);
        return false;
    }

    UAutoChatVoiceInputComponent* VoiceComp = NewObject<UAutoChatVoiceInputComponent>(PlayerActor);
    VoiceComp->RegisterComponent();
    VoiceComp->MaxInteractionRange = 300.0f;

    const bool bOk = VoiceComp->SubmitRecognizedSpeech(TEXT("Can anyone hear me?"));
    TestFalse(TEXT("SubmitRecognizedSpeech should fail when no AI is in hearing range."), bOk);

    const TArray<FLocalTalkMessage> History = Sub->GetContextHistory(AI);
    TestEqual(TEXT("Out-of-range should not write any user history."), History.Num(), 0);

    PlayerActor->Destroy();
    CleanupTalkers(Sub, AIActor, AI);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAutoChatVoiceInputRepetitionSpamGuardTest,
    "Project.AutoChat.VoiceInput.RepetitionSpamGuardRejectsLoopingTranscript",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutoChatVoiceInputRepetitionSpamGuardTest::RunTest(const FString& Parameters)
{
    UWorld* World = GetAutomationWorld();
    if (!World)
    {
        AddError(TEXT("Editor world not found."));
        return false;
    }

    ULocalTalkConversationSubsystem* Sub = World->GetSubsystem<ULocalTalkConversationSubsystem>();
    if (!Sub)
    {
        AddError(TEXT("Conversation subsystem not found."));
        return false;
    }

    AActor* AIActor = nullptr;
    ULocalCharacterComponent* AI = nullptr;
    const FVector Base(640000.0f, 155000.0f, 100.0f);
    if (!SpawnTalker(*this, World, Sub, Base + FVector(200.0f, 0.0f, 0.0f), TEXT("Guard"), AIActor, AI))
    {
        CleanupTalkers(Sub, AIActor, AI);
        return false;
    }

    AActor* PlayerActor = SpawnRootedActor(World, Base);
    if (!PlayerActor)
    {
        AddError(TEXT("Failed to spawn player actor."));
        CleanupTalkers(Sub, AIActor, AI);
        return false;
    }

    UAutoChatVoiceInputComponent* VoiceComp = NewObject<UAutoChatVoiceInputComponent>(PlayerActor);
    VoiceComp->RegisterComponent();
    VoiceComp->MaxInteractionRange = 1200.0f;

    FString SpamTranscript;
    for (int32 i = 0; i < 8; ++i)
    {
        SpamTranscript += TEXT("regroup at the bridge. ");
    }

    const bool bAccepted = VoiceComp->SubmitRecognizedSpeech(SpamTranscript);
    TestFalse(TEXT("Looping transcript should be rejected by repetition spam guard."), bAccepted);

    const TArray<FLocalTalkMessage> History = Sub->GetContextHistory(AI);
    TestEqual(TEXT("Rejected looping transcript should not enter history."), History.Num(), 0);

    PlayerActor->Destroy();
    CleanupTalkers(Sub, AIActor, AI);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAutoChatVoiceInputSingleTargetRotationTest,
    "Project.AutoChat.VoiceInput.RotateSingleTargetAcrossNearbyAIs",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutoChatVoiceInputSingleTargetRotationTest::RunTest(const FString& Parameters)
{
    UWorld* World = GetAutomationWorld();
    if (!World)
    {
        AddError(TEXT("Editor world not found."));
        return false;
    }

    ULocalTalkConversationSubsystem* Sub = World->GetSubsystem<ULocalTalkConversationSubsystem>();
    if (!Sub)
    {
        AddError(TEXT("Conversation subsystem not found."));
        return false;
    }

    AActor* NearActor = nullptr;
    AActor* FarActor = nullptr;
    ULocalCharacterComponent* NearAI = nullptr;
    ULocalCharacterComponent* FarAI = nullptr;

    const FVector Base(710000.0f, 161000.0f, 100.0f);
    if (!SpawnTalker(*this, World, Sub, Base + FVector(400.0f, 0.0f, 0.0f), TEXT("NearAI"), NearActor, NearAI) ||
        !SpawnTalker(*this, World, Sub, Base + FVector(-700.0f, 0.0f, 0.0f), TEXT("FarAI"), FarActor, FarAI))
    {
        CleanupTalkers(Sub, NearActor, NearAI, FarActor, FarAI);
        return false;
    }

    AActor* PlayerActor = SpawnRootedActor(World, Base);
    if (!PlayerActor)
    {
        AddError(TEXT("Failed to spawn player actor."));
        CleanupTalkers(Sub, NearActor, NearAI, FarActor, FarAI);
        return false;
    }

    UAutoChatVoiceInputComponent* VoiceComp = NewObject<UAutoChatVoiceInputComponent>(PlayerActor);
    VoiceComp->RegisterComponent();
    VoiceComp->MaxInteractionRange = 1400.0f;
    VoiceComp->bBroadcastToAllNearby = false;
    VoiceComp->bRotateSingleTargetAcrossNearby = true;

    const TArray<FString> Prompts =
    {
        TEXT("First player update about supplies."),
        TEXT("Second player update about route planning."),
        TEXT("Third player update about timing."),
        TEXT("Fourth player update about fallback plans.")
    };

    for (const FString& Prompt : Prompts)
    {
        const bool bOk = VoiceComp->SubmitRecognizedSpeech(Prompt);
        TestTrue(TEXT("Rotating single-target routing should accept valid transcript."), bOk);
    }

    const int32 NearUserLines = Algo::CountIf(Sub->GetContextHistory(NearAI), [](const FLocalTalkMessage& M) { return M.bFromUser; });
    const int32 FarUserLines = Algo::CountIf(Sub->GetContextHistory(FarAI), [](const FLocalTalkMessage& M) { return M.bFromUser; });

    TestEqual(TEXT("Near AI should receive two of four rotating user turns."), NearUserLines, 2);
    TestEqual(TEXT("Far AI should receive two of four rotating user turns."), FarUserLines, 2);

    PlayerActor->Destroy();
    CleanupTalkers(Sub, NearActor, NearAI, FarActor, FarAI);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAutoChatVoiceInputSubmitCancelsQueuedNpcTurnsTest,
    "Project.AutoChat.VoiceInput.SubmitCancelsNearbyQueuedNpcTurns",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutoChatVoiceInputSubmitCancelsQueuedNpcTurnsTest::RunTest(const FString& Parameters)
{
    UWorld* World = GetAutomationWorld();
    if (!World)
    {
        AddError(TEXT("Editor world not found."));
        return false;
    }

    ULocalTalkConversationSubsystem* Sub = World->GetSubsystem<ULocalTalkConversationSubsystem>();
    if (!Sub)
    {
        AddError(TEXT("Conversation subsystem not found."));
        return false;
    }

    AActor* PrimaryActor = nullptr;
    AActor* SecondaryActor = nullptr;
    ULocalCharacterComponent* PrimaryAI = nullptr;
    ULocalCharacterComponent* SecondaryAI = nullptr;

    const FVector Base(780000.0f, 168000.0f, 100.0f);
    if (!SpawnTalker(*this, World, Sub, Base + FVector(150.0f, 0.0f, 0.0f), TEXT("Primary"), PrimaryActor, PrimaryAI) ||
        !SpawnTalker(*this, World, Sub, Base + FVector(700.0f, 0.0f, 0.0f), TEXT("Secondary"), SecondaryActor, SecondaryAI))
    {
        CleanupTalkers(Sub, PrimaryActor, PrimaryAI, SecondaryActor, SecondaryAI);
        return false;
    }

    AActor* PlayerActor = SpawnRootedActor(World, Base);
    if (!PlayerActor)
    {
        AddError(TEXT("Failed to spawn player actor."));
        CleanupTalkers(Sub, PrimaryActor, PrimaryAI, SecondaryActor, SecondaryAI);
        return false;
    }

    UAutoChatVoiceInputComponent* VoiceComp = NewObject<UAutoChatVoiceInputComponent>(PlayerActor);
    VoiceComp->RegisterComponent();
    VoiceComp->MaxInteractionRange = 1400.0f;
    VoiceComp->bBroadcastToAllNearby = false;
    VoiceComp->bRotateSingleTargetAcrossNearby = false;
    VoiceComp->bCancelQueuedTurnsOnVoiceCaptureStart = true;
    VoiceComp->bInterruptNearbyAIOnVoiceCaptureStart = true;

    Sub->RequestTurn(SecondaryAI, TEXT("Director instruction: Continue the side conversation."), /*bFromUser*/false);
    TestEqual(TEXT("Precondition: one queued NPC turn should exist before player transcript submit."),
        Sub->Test_GetManualQueueSize(), 1);
    TestEqual(TEXT("Precondition: queued turn should be NPC-authored."),
        Sub->Test_GetManualQueueNpcCount(), 1);

    const bool bOk = VoiceComp->SubmitRecognizedSpeech(TEXT("Hold on, answer me first."));
    TestTrue(TEXT("Player transcript should be accepted and routed."), bOk);

    TestEqual(TEXT("Queued NPC turns near the player should be removed after submit."), Sub->Test_GetManualQueueNpcCount(), 0);
    TestEqual(TEXT("Only one user turn should remain queued after submit."), Sub->Test_GetManualQueueUserCount(), 1);
    TestEqual(TEXT("Total queued turns should collapse to one user-priority turn."), Sub->Test_GetManualQueueSize(), 1);

    const TArray<FLocalTalkMessage> PrimaryHistory = Sub->GetContextHistory(PrimaryAI);
    TestTrue(TEXT("Primary target history should include the submitted player line."),
        PrimaryHistory.Num() > 0 && PrimaryHistory.Last().bFromUser);

    PlayerActor->Destroy();
    CleanupTalkers(Sub, PrimaryActor, PrimaryAI, SecondaryActor, SecondaryAI);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAutoChatVoiceInputDropAIAudibleSegmentWithoutBargeInTest,
    "Project.AutoChat.VoiceInput.DropsAIAudibleSegmentWithoutBargeIn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAutoChatVoiceInputDropAIAudibleSegmentWithoutBargeInTest::RunTest(const FString& Parameters)
{
    UWorld* World = GetAutomationWorld();
    if (!World)
    {
        AddError(TEXT("Editor world not found."));
        return false;
    }

    AActor* PlayerActor = SpawnRootedActor(World, FVector(820000.0f, 176000.0f, 100.0f));
    if (!PlayerActor)
    {
        AddError(TEXT("Failed to spawn player actor."));
        return false;
    }

    UAutoChatVoiceInputComponent* VoiceComp = NewObject<UAutoChatVoiceInputComponent>(PlayerActor);
    if (!VoiceComp)
    {
        AddError(TEXT("Failed to create UAutoChatVoiceInputComponent."));
        PlayerActor->Destroy();
        return false;
    }
    VoiceComp->RegisterComponent();

    // Deterministic always-on test setup with direct calls into segment logic.
    VoiceComp->Test_SetMicCaptureActive(true);
    VoiceComp->bAlwaysOnAutoTranscribe = true;
    VoiceComp->bDropAIAudibleSegmentsWithoutBargeIn = true;
    VoiceComp->bBargeInRequiresAudibleAIVoice = false;
    VoiceComp->bUseAdaptiveNoiseFloor = false;
    VoiceComp->AutoTranscribeStartRmsThreshold = 0.02f;
    VoiceComp->AutoTranscribeContinueRmsThreshold = 0.015f;
    VoiceComp->AutoTranscribeStartHoldSeconds = 0.01f;
    VoiceComp->AutoTranscribeSilenceSeconds = 0.0f;
    VoiceComp->AutoTranscribeMinSpeechSeconds = 0.0f;
    VoiceComp->AutoTranscribeMinActiveSpeechSeconds = 0.0f;
    VoiceComp->AutoTranscribeMinActiveRatio = 0.0f;

    const int32 SampleRate = 1000;
    const int32 NumChannels = 1;
    const int32 NumFrames = 32;

    TArray<float> ActiveChunk;
    ActiveChunk.Init(0.10f, NumFrames * NumChannels);
    VoiceComp->Test_ProcessAlwaysOnChunk(ActiveChunk.GetData(), NumFrames, NumChannels, SampleRate, 0.10f);

    TestTrue(TEXT("Always-on segment should have started."), VoiceComp->Test_IsAlwaysOnSegmentActive());

    // Simulate AI-audible overlap without true user barge-in.
    VoiceComp->Test_SetAlwaysOnSegmentHadAudibleAI(true);
    VoiceComp->Test_SetAlwaysOnSegmentBargeInTriggered(false);

    TArray<float> SilentChunk;
    SilentChunk.Init(0.0f, NumFrames * NumChannels);
    VoiceComp->Test_ProcessAlwaysOnChunk(SilentChunk.GetData(), NumFrames, NumChannels, SampleRate, 0.0f);

    TestFalse(TEXT("Segment should end after silence."), VoiceComp->Test_IsAlwaysOnSegmentActive());
    TestFalse(TEXT("AI-overlap segment without barge-in should not queue Whisper request."), VoiceComp->Test_IsAutoTranscribeRequestInFlight());

    PlayerActor->Destroy();
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
