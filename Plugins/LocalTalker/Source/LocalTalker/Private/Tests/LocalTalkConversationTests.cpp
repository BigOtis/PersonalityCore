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

FString NormalizeLineForTranscriptChecks(const FString& In)
{
    FString S = In.ToLower();
    S.ReplaceInline(TEXT("\r"), TEXT(" "));
    S.ReplaceInline(TEXT("\n"), TEXT(" "));
    S.ReplaceInline(TEXT("\t"), TEXT(" "));
    for (int32 i = 0; i < S.Len(); ++i)
    {
        const TCHAR C = S[i];
        if (!FChar::IsAlnum(C) && !FChar::IsWhitespace(C))
        {
            S[i] = TEXT(' ');
        }
    }
    while (S.Contains(TEXT("  ")))
    {
        S.ReplaceInline(TEXT("  "), TEXT(" "));
    }
    S.TrimStartAndEndInline();
    return S;
}

TSet<FString> ExtractKeywords(const FString& In)
{
    static const TSet<FString> Stop =
    {
        TEXT("the"), TEXT("and"), TEXT("that"), TEXT("this"), TEXT("with"),
        TEXT("what"), TEXT("when"), TEXT("where"), TEXT("which"), TEXT("have"),
        TEXT("from"), TEXT("your"), TEXT("about"), TEXT("into"), TEXT("plan"),
        TEXT("will"), TEXT("should"), TEXT("could"), TEXT("would"), TEXT("there"),
        TEXT("they"), TEXT("them"), TEXT("then"), TEXT("just"), TEXT("more"),
        TEXT("does"), TEXT("need"), TEXT("enough"), TEXT("for"), TEXT("are")
    };

    TSet<FString> Out;
    TArray<FString> Tokens;
    NormalizeLineForTranscriptChecks(In).ParseIntoArrayWS(Tokens);
    for (const FString& Token : Tokens)
    {
        if (Token.Len() < 4)
        {
            continue;
        }
        if (!Stop.Contains(Token))
        {
            Out.Add(Token);
        }
    }
    return Out;
}

bool LineAcknowledgesUserIntent(const FString& UserLine, const FString& NpcLine)
{
    const FString NpcNorm = NormalizeLineForTranscriptChecks(NpcLine);
    if (NpcNorm.IsEmpty())
    {
        return false;
    }

    const TSet<FString> Keywords = ExtractKeywords(UserLine);
    for (const FString& Keyword : Keywords)
    {
        if (NpcNorm.Contains(Keyword))
        {
            return true;
        }
    }

    return NpcNorm.StartsWith(TEXT("yes ")) ||
           NpcNorm.StartsWith(TEXT("no ")) ||
           NpcNorm.Contains(TEXT("yes,")) ||
           NpcNorm.Contains(TEXT("no,"));
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
    Sub->RequestTurn(Alice, PlayerPrompt, /*bFromUser*/true);

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLocalTalkConversationUserTurnUpgradesQueuedNpcTurnTest,
    "Plugins.LocalTalker.Dialog.E2E.UserTurnUpgradesQueuedNpcTurn",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLocalTalkConversationUserTurnUpgradesQueuedNpcTurnTest::RunTest(const FString& Parameters)
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

    AActor* Actor = nullptr;
    ULocalCharacterComponent* Talker = nullptr;
    const FVector Base(940000.0f, 310000.0f, 100.0f);
    if (!SpawnTalker(*this, World, Sub, Base, TEXT("Responder"), Actor, Talker))
    {
        CleanupTalkers(Sub, Actor, Talker);
        return false;
    }

    Sub->ClearContextHistory(Talker);
    Sub->RequestTurn(Talker, TEXT("Director instruction: give a side update."), /*bFromUser*/false);
    TestEqual(TEXT("One queued turn should exist after NPC request."), Sub->Test_GetManualQueueSize(), 1);
    TestEqual(TEXT("Queued turn should currently be NPC."), Sub->Test_GetManualQueueNpcCount(), 1);

    const FString PlayerPrompt = TEXT("Please answer my question directly.");
    Sub->RequestTurn(Talker, PlayerPrompt, /*bFromUser*/true);

    TestEqual(TEXT("Queue size should stay at one turn for same talker."), Sub->Test_GetManualQueueSize(), 1);
    TestEqual(TEXT("NPC queue entry should be upgraded to a user turn."), Sub->Test_GetManualQueueNpcCount(), 0);
    TestEqual(TEXT("Exactly one queued user turn should remain."), Sub->Test_GetManualQueueUserCount(), 1);

    const TArray<FLocalTalkMessage> History = Sub->GetContextHistory(Talker);
    TestEqual(TEXT("User prompt should be recorded in history immediately."), History.Num(), 1);
    if (History.Num() == 1)
    {
        TestTrue(TEXT("Recorded prompt should be marked as user input."), History[0].bFromUser);
        TestEqual(TEXT("Recorded user content should match latest prompt."), History[0].Content, PlayerPrompt);
    }

    CleanupTalkers(Sub, Actor, Talker);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLocalTalkConversationPromptPlayerFirstContractTest,
    "Plugins.LocalTalker.Dialog.E2E.PromptPlayerFirstContract",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLocalTalkConversationPromptPlayerFirstContractTest::RunTest(const FString& Parameters)
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

    const FVector Base(980000.0f, 320000.0f, 100.0f);
    if (!SpawnTalker(*this, World, Sub, Base, TEXT("Alice"), AliceActor, Alice) ||
        !SpawnTalker(*this, World, Sub, Base + FVector(100.0f, 0.0f, 0.0f), TEXT("Bob"), BobActor, Bob))
    {
        CleanupTalkers(Sub, AliceActor, Alice, BobActor, Bob);
        return false;
    }

    TArray<FLocalTalkMessage> History;
    History.Add({ TEXT("User"), TEXT("Can we cross the bridge now or should we wait?"), true });
    History.Add({ TEXT("Bob"), TEXT("The wind is still strong but the ropes look stable."), false });

    TArray<ULocalCharacterComponent*> Participants;
    Participants.Add(Alice);
    Participants.Add(Bob);

    const FString Prompt = Alice->Test_BuildLlama3PromptFromContext(
        History,
        Participants,
        TEXT("Can we cross the bridge now or should we wait?"));

    TestTrue(TEXT("Prompt must include player-first rule for question/request responses."),
        Prompt.Contains(TEXT("If the latest [PLAYER] line is a question or request, answer it directly in sentence 1.")));
    TestTrue(TEXT("Prompt must include unanswered-player guard."),
        Prompt.Contains(TEXT("Do not skip or talk past unanswered player questions.")));
    TestTrue(TEXT("Prompt must cap follow-up questions to reduce loops."),
        Prompt.Contains(TEXT("Ask at most one short question")));
    TestTrue(TEXT("Prompt should include the current player message in tagged form."),
        Prompt.Contains(TEXT("[PLAYER] Can we cross the bridge now or should we wait? [/PLAYER]")));

    CleanupTalkers(Sub, AliceActor, Alice, BobActor, Bob);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FLocalTalkConversationMockedTranscriptQualityTest,
    "Plugins.LocalTalker.Dialog.E2E.MockedTranscriptQualityHeuristic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FLocalTalkConversationMockedTranscriptQualityTest::RunTest(const FString& Parameters)
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

    const FVector Base(1020000.0f, 330000.0f, 100.0f);
    if (!SpawnTalker(*this, World, Sub, Base, TEXT("Alice"), AliceActor, Alice) ||
        !SpawnTalker(*this, World, Sub, Base + FVector(120.0f, 0.0f, 0.0f), TEXT("Bob"), BobActor, Bob))
    {
        CleanupTalkers(Sub, AliceActor, Alice, BobActor, Bob);
        return false;
    }

    Sub->ClearContextHistory(Alice);

    Sub->RequestTurn(Alice, TEXT("We need a safe canyon route before dusk. What's the best plan?"), /*bFromUser*/true);
    Sub->BroadcastSentence(Alice, TEXT("For this canyon route, let's use the east ridge because the crosswind is weaker there."), false);
    Sub->BroadcastSentence(Bob, TEXT("Agreed, and I'll mark that ridge route with flares every fifty meters so we stay aligned."), false);
    Sub->RequestTurn(Bob, TEXT("Do we have enough batteries for those flares overnight?"), /*bFromUser*/true);
    Sub->BroadcastSentence(Alice, TEXT("Yes, we packed three spare batteries, so the flares can run through the night shift."), false);
    Sub->BroadcastSentence(Bob, TEXT("Then we keep one battery in reserve and move at dusk to avoid the strongest gusts."), false);

    const TArray<FLocalTalkMessage> History = Sub->GetContextHistory(Alice);
    TestTrue(TEXT("Mocked transcript should contain at least six lines."), History.Num() >= 6);

    bool bHasAdjacentDuplicate = false;
    for (int32 i = 1; i < History.Num(); ++i)
    {
        const FString Prev = NormalizeLineForTranscriptChecks(History[i - 1].Content);
        const FString Curr = NormalizeLineForTranscriptChecks(History[i].Content);
        if (!Prev.IsEmpty() && Prev.Equals(Curr, ESearchCase::CaseSensitive))
        {
            bHasAdjacentDuplicate = true;
            break;
        }
    }
    TestFalse(TEXT("Transcript should avoid adjacent duplicate lines."), bHasAdjacentDuplicate);

    bool bAllUserTurnsAcknowledged = true;
    for (int32 i = 0; i < History.Num(); ++i)
    {
        if (!History[i].bFromUser)
        {
            continue;
        }

        bool bAcknowledged = false;
        for (int32 j = i + 1; j < History.Num() && j <= i + 2; ++j)
        {
            if (History[j].bFromUser)
            {
                continue;
            }
            if (LineAcknowledgesUserIntent(History[i].Content, History[j].Content))
            {
                bAcknowledged = true;
                break;
            }
        }

        if (!bAcknowledged)
        {
            bAllUserTurnsAcknowledged = false;
            break;
        }
    }

    TestTrue(TEXT("Each user turn should be acknowledged by nearby NPC responses."), bAllUserTurnsAcknowledged);

    CleanupTalkers(Sub, AliceActor, Alice, BobActor, Bob);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
