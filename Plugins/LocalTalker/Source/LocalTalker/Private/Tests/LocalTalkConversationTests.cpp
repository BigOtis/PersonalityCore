// Automation tests for LocalTalkConversationSubsystem
#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "LocalTalkConversationSubsystem.h"
#include "LocalCharacterComponent.h"
#include "LocalPlayerInteractionComponent.h"
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

    OutComponent->ConversationRadius = 500.0f;
    OutComponent->SpeakerName = SpeakerName;
    OutComponent->RegisterComponent();
    Sub->RegisterTalker(OutComponent);
    return true;
}

void CleanupTalkers(
    ULocalTalkConversationSubsystem* Sub,
    AActor* A1, ULocalCharacterComponent* C1,
    AActor* A2 = nullptr, ULocalCharacterComponent* C2 = nullptr,
    AActor* A3 = nullptr, ULocalCharacterComponent* C3 = nullptr)
{
    if (Sub)
    {
        if (C1) Sub->UnregisterTalker(C1);
        if (C2) Sub->UnregisterTalker(C2);
        if (C3) Sub->UnregisterTalker(C3);
    }

    if (A1) A1->Destroy();
    if (A2) A2->Destroy();
    if (A3) A3->Destroy();
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLocalTalkConversationTwoCharacterFlowTest,
    "Plugins.LocalTalker.Dialog.E2E.SpawnedTwoCharacterConversation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLocalTalkConversationTwoCharacterFlowTest::RunTest(const FString& Parameters)
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

    AActor* AliceActor = nullptr;
    AActor* BobActor = nullptr;
    ULocalCharacterComponent* Alice = nullptr;
    ULocalCharacterComponent* Bob = nullptr;

    const FVector Base(10000.0f, 5000.0f, 100.0f);
    if (!SpawnTalker(*this, World, Sub, Base, TEXT("Alice"), AliceActor, Alice) ||
        !SpawnTalker(*this, World, Sub, Base + FVector(120.0f, 0.0f, 0.0f), TEXT("Bob"), BobActor, Bob))
    {
        CleanupTalkers(Sub, AliceActor, Alice, BobActor, Bob);
        return false;
    }

    Sub->ClearContextHistory(Alice);
    const TArray<ULocalCharacterComponent*> Participants = Sub->GetContextParticipants(Alice);
    TestTrue(TEXT("Alice should see Bob in her conversation context."), Participants.Contains(Bob));

    Sub->BroadcastSentence(Alice, TEXT("Hello Bob, did you map the cave entrance?"), false);
    Sub->BroadcastSentence(Bob, TEXT("Yes, but there is a blocked path near the north wall."), false);

    const TArray<FLocalTalkMessage> History = Sub->GetContextHistory(Alice);
    TestEqual(TEXT("Conversation history should contain two messages."), History.Num(), 2);
    if (History.Num() == 2)
    {
        TestEqual(TEXT("First speaker should be Alice."), History[0].SpeakerName, FString(TEXT("Alice")));
        TestEqual(TEXT("Second speaker should be Bob."), History[1].SpeakerName, FString(TEXT("Bob")));
        TestFalse(TEXT("NPC lines should not be marked as user input."), History[0].bFromUser);
        TestFalse(TEXT("NPC lines should not be marked as user input."), History[1].bFromUser);
    }

    CleanupTalkers(Sub, AliceActor, Alice, BobActor, Bob);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLocalTalkConversationPlayerInputReactionTest,
    "Plugins.LocalTalker.Dialog.E2E.PlayerInputReaction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLocalTalkConversationPlayerInputReactionTest::RunTest(const FString& Parameters)
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

    AActor* AliceActor = nullptr;
    AActor* BobActor = nullptr;
    ULocalCharacterComponent* Alice = nullptr;
    ULocalCharacterComponent* Bob = nullptr;

    const FVector Base(20000.0f, 5000.0f, 100.0f);
    if (!SpawnTalker(*this, World, Sub, Base, TEXT("Alice"), AliceActor, Alice) ||
        !SpawnTalker(*this, World, Sub, Base + FVector(80.0f, 0.0f, 0.0f), TEXT("Bob"), BobActor, Bob))
    {
        CleanupTalkers(Sub, AliceActor, Alice, BobActor, Bob);
        return false;
    }

    const FString PlayerPrompt = TEXT("What should we do before entering the cave?");
    Sub->RequestTurn(Alice, PlayerPrompt);

    TArray<FLocalTalkMessage> History = Sub->GetContextHistory(Alice);
    TestEqual(TEXT("RequestTurn should record user input immediately."), History.Num(), 1);
    if (History.Num() == 1)
    {
        TestEqual(TEXT("User line speaker should be tagged as User."), History[0].SpeakerName, FString(TEXT("User")));
        TestEqual(TEXT("Recorded user prompt should match input text."), History[0].Content, PlayerPrompt);
        TestTrue(TEXT("User prompt should be marked as bFromUser."), History[0].bFromUser);
    }

    Sub->BroadcastSentence(Alice, TEXT("We should light a torch and check the rope knots first."), false);
    Sub->BroadcastSentence(Bob, TEXT("And mark the route so we can return safely."), false);

    History = Sub->GetContextHistory(Bob);
    TestEqual(TEXT("History should include user prompt plus two NPC responses."), History.Num(), 3);
    if (History.Num() == 3)
    {
        TestTrue(TEXT("First history entry remains the player prompt."), History[0].bFromUser);
        TestEqual(TEXT("Second speaker should be Alice's response."), History[1].SpeakerName, FString(TEXT("Alice")));
        TestEqual(TEXT("Third speaker should be Bob's response."), History[2].SpeakerName, FString(TEXT("Bob")));
    }

    CleanupTalkers(Sub, AliceActor, Alice, BobActor, Bob);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLocalTalkConversationDistanceIsolationTest,
    "Plugins.LocalTalker.Dialog.E2E.ContextIsolationByDistance",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLocalTalkConversationDistanceIsolationTest::RunTest(const FString& Parameters)
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

    AActor* AliceActor = nullptr;
    AActor* BobActor = nullptr;
    AActor* CharlieActor = nullptr;
    ULocalCharacterComponent* Alice = nullptr;
    ULocalCharacterComponent* Bob = nullptr;
    ULocalCharacterComponent* Charlie = nullptr;

    const FVector Base(800000.0f, 300000.0f, 100.0f);
    if (!SpawnTalker(*this, World, Sub, Base, TEXT("Alice"), AliceActor, Alice) ||
        !SpawnTalker(*this, World, Sub, Base + FVector(150.0f, 0.0f, 0.0f), TEXT("Bob"), BobActor, Bob) ||
        !SpawnTalker(*this, World, Sub, Base + FVector(5000.0f, 0.0f, 0.0f), TEXT("Charlie"), CharlieActor, Charlie))
    {
        CleanupTalkers(Sub, AliceActor, Alice, BobActor, Bob, CharlieActor, Charlie);
        return false;
    }

    Sub->ClearContextHistory(Alice);
    Sub->ClearContextHistory(Charlie);

    Sub->BroadcastSentence(Alice, TEXT("Bob, do you still have the spare battery?"), false);

    const TArray<FLocalTalkMessage> NearHistory = Sub->GetContextHistory(Bob);
    const TArray<FLocalTalkMessage> FarHistory = Sub->GetContextHistory(Charlie);
    const TArray<ULocalCharacterComponent*> CharlieParticipants = Sub->GetContextParticipants(Charlie);

    TestEqual(TEXT("Nearby participant should see the message history."), NearHistory.Num(), 1);
    TestEqual(TEXT("Far participant should stay in an isolated context."), FarHistory.Num(), 0);
    TestTrue(TEXT("Far context should include Charlie."), CharlieParticipants.Contains(Charlie));
    TestFalse(TEXT("Far context should not include Alice."), CharlieParticipants.Contains(Alice));
    TestFalse(TEXT("Far context should not include Bob."), CharlieParticipants.Contains(Bob));

    CleanupTalkers(Sub, AliceActor, Alice, BobActor, Bob, CharlieActor, Charlie);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLocalTalkPlayerInteractionComponentTest,
    "Plugins.LocalTalker.Dialog.E2E.PlayerInteractionComponentRoutesToAI",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLocalTalkPlayerInteractionComponentTest::RunTest(const FString& Parameters)
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

    AActor* AliceActor = nullptr;
    ULocalCharacterComponent* Alice = nullptr;
    const FVector AIBase(900000.0f, 300000.0f, 100.0f);
    if (!SpawnTalker(*this, World, Sub, AIBase, TEXT("Alice"), AliceActor, Alice))
    {
        CleanupTalkers(Sub, AliceActor, Alice);
        return false;
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AActor* PlayerActor = World->SpawnActor<AActor>(AActor::StaticClass(), AIBase + FVector(50.0f, 0.0f, 0.0f), FRotator::ZeroRotator, SpawnParams);
    if (!PlayerActor)
    {
        AddError(TEXT("Failed to spawn player actor."));
        CleanupTalkers(Sub, AliceActor, Alice);
        return false;
    }
    if (!PlayerActor->GetRootComponent())
    {
        USceneComponent* Root = NewObject<USceneComponent>(PlayerActor, TEXT("PlayerTestRoot"));
        if (Root)
        {
            Root->RegisterComponent();
            PlayerActor->SetRootComponent(Root);
        }
    }
    PlayerActor->SetActorLocation(AIBase + FVector(50.0f, 0.0f, 0.0f));

    ULocalPlayerInteractionComponent* PlayerInteraction = NewObject<ULocalPlayerInteractionComponent>(PlayerActor);
    if (!PlayerInteraction)
    {
        AddError(TEXT("Failed to create LocalPlayerInteractionComponent."));
        PlayerActor->Destroy();
        CleanupTalkers(Sub, AliceActor, Alice);
        return false;
    }
    PlayerInteraction->InteractionRange = 500.0f;
    PlayerInteraction->RegisterComponent();

    const FString PlayerPrompt = TEXT("Can you summarize the objective in one line?");
    Sub->ClearContextHistory(Alice);
    const bool bSent = PlayerInteraction->SpeakToNearestAI(PlayerPrompt);
    TestTrue(TEXT("Player interaction component should submit prompt to nearest AI."), bSent);

    const TArray<FLocalTalkMessage> History = Sub->GetContextHistory(Alice);
    TestEqual(TEXT("History should contain user input routed through player component."), History.Num(), 1);
    if (History.Num() == 1)
    {
        TestTrue(TEXT("History entry should be marked as user input."), History[0].bFromUser);
        TestEqual(TEXT("History text should match player prompt."), History[0].Content, PlayerPrompt);
    }

    PlayerActor->Destroy();
    CleanupTalkers(Sub, AliceActor, Alice);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
