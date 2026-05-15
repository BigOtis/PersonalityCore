#include "ConventionBotSubsystem.h"

#include "Animation/AnimationAsset.h"
#include "CollisionQueryParams.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "LocalCharacterComponent.h"
#include "Materials/MaterialInterface.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
static const FName ConventionBotTag(TEXT("ConventionBot"));
}

void UConventionBotSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    EnsureAssetsLoaded();
}

void UConventionBotSubsystem::Tick(float DeltaTime)
{
    EnsureAssetsLoaded();
    RefreshBots();

    for (FConventionBotState& Bot : Bots)
    {
        if (!Bot.Actor.IsValid() || !Bot.LocalCharacter.IsValid() || !Bot.Mesh.IsValid())
        {
            continue;
        }

        if (!Bot.bInitialized)
        {
            InitializeBot(Bot);
        }

        UpdateRoaming(Bot, DeltaTime);
        UpdateFacing(Bot, DeltaTime);
        SnapToGround(Bot, DeltaTime);
        UpdateAnimation(Bot, DeltaTime);
    }
}

TStatId UConventionBotSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UConventionBotSubsystem, STATGROUP_Tickables);
}

bool UConventionBotSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
    return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UConventionBotSubsystem::EnsureAssetsLoaded()
{
    if (MannyMesh)
    {
        return;
    }

    MannyMesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny.SKM_Manny"));
    QuinnMesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Characters/Mannequins/Meshes/SKM_Quinn.SKM_Quinn"));
    MannyIdleAnimation = LoadObject<UAnimationAsset>(nullptr, TEXT("/Game/Characters/Mannequins/Animations/Manny/MM_Idle.MM_Idle"));
    MannyWalkAnimation = LoadObject<UAnimationAsset>(nullptr, TEXT("/Game/Characters/Mannequins/Animations/Manny/MM_Walk_InPlace.MM_Walk_InPlace"));
    MannyTalkAnimation = LoadObject<UAnimationAsset>(nullptr, TEXT("/Game/Characters/Mannequins/Animations/Manny/MM_Idle.MM_Idle"));
    QuinnIdleAnimation = LoadObject<UAnimationAsset>(nullptr, TEXT("/Game/Characters/Mannequins/Animations/Quinn/MF_Idle.MF_Idle"));
    QuinnWalkAnimation = LoadObject<UAnimationAsset>(nullptr, TEXT("/Game/Characters/Mannequins/Animations/Quinn/MF_Walk_Fwd.MF_Walk_Fwd"));
    QuinnTalkAnimation = LoadObject<UAnimationAsset>(nullptr, TEXT("/Game/Characters/Mannequins/Animations/Quinn/MF_Idle.MF_Idle"));
    MannyMaterialA = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Characters/Mannequins/Materials/Instances/Manny/MI_Manny_01.MI_Manny_01"));
    MannyMaterialB = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Characters/Mannequins/Materials/Instances/Manny/MI_Manny_02.MI_Manny_02"));
    QuinnMaterialA = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Characters/Mannequins/Materials/Instances/Quinn/MI_Quinn_01.MI_Quinn_01"));
    QuinnMaterialB = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Characters/Mannequins/Materials/Instances/Quinn/MI_Quinn_02.MI_Quinn_02"));
}

void UConventionBotSubsystem::RefreshBots(bool bForce)
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    const double Now = World->GetTimeSeconds();
    if (!bForce && Now < NextRefreshTimeSeconds)
    {
        return;
    }
    NextRefreshTimeSeconds = Now + 1.0;

    TArray<FConventionBotState> NewBots;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!Actor || !Actor->Tags.Contains(ConventionBotTag))
        {
            continue;
        }

        ULocalCharacterComponent* LocalCharacter = Actor->FindComponentByClass<ULocalCharacterComponent>();
        USkeletalMeshComponent* Mesh = Actor->FindComponentByClass<USkeletalMeshComponent>();
        if (!LocalCharacter || !Mesh)
        {
            continue;
        }

        FConventionBotState Bot;
        Bot.Actor = Actor;
        Bot.LocalCharacter = LocalCharacter;
        Bot.Mesh = Mesh;
        Bot.BodyType = ReadStringTag(Actor, TEXT("CB_Body="), TEXT("Manny")).Equals(TEXT("Quinn"), ESearchCase::IgnoreCase)
            ? EBodyType::Quinn
            : EBodyType::Manny;
        Bot.OutfitVariant = ReadIntTag(Actor, TEXT("CB_Outfit="), 0);
        Bot.bRoam = ReadBoolTag(Actor, TEXT("CB_Roam="), false);
        Bot.PatrolOffsets = ReadPatrolOffsets(Actor);
        Bot.RoamSpeed = ReadFloatTag(Actor, TEXT("CB_RoamSpeed="), 150.0f);
        Bot.FacePlayerRadius = ReadFloatTag(Actor, TEXT("CB_FacePlayerRadius="), 850.0f);
        Bot.FaceBotRadius = ReadFloatTag(Actor, TEXT("CB_FaceBotRadius="), 900.0f);
        Bot.TalkAnimRate = ReadFloatTag(Actor, TEXT("CB_TalkRate="), 0.35f);
        Bot.WalkAnimRate = ReadFloatTag(Actor, TEXT("CB_WalkRate="), 1.0f);

        for (FConventionBotState& Existing : Bots)
        {
            if (Existing.Actor == Bot.Actor)
            {
                Bot.PatrolIndex = Existing.PatrolIndex;
                Bot.PatrolOrigin = Existing.PatrolOrigin;
                Bot.PauseRemaining = Existing.PauseRemaining;
                Bot.bInitialized = Existing.bInitialized;
                Bot.bHasGroundOffset = Existing.bHasGroundOffset;
                Bot.bMovingLastTick = Existing.bMovingLastTick;
                Bot.GroundOffsetZ = Existing.GroundOffsetZ;
                Bot.BaseActorRotation = Existing.BaseActorRotation;
                Bot.MeshBaseRelativeLocation = Existing.MeshBaseRelativeLocation;
                Bot.MeshBaseRelativeRotation = Existing.MeshBaseRelativeRotation;
                Bot.ActiveAnimation = Existing.ActiveAnimation;
                Bot.TalkPoseTime = Existing.TalkPoseTime;
                break;
            }
        }

        NewBots.Add(MoveTemp(Bot));
    }

    Bots = MoveTemp(NewBots);
}

void UConventionBotSubsystem::ApplyVisuals(FConventionBotState& Bot)
{
    if (!Bot.Mesh.IsValid())
    {
        return;
    }

    USkeletalMesh* MeshAsset = (Bot.BodyType == EBodyType::Quinn) ? QuinnMesh : MannyMesh;
    UMaterialInterface* Material = nullptr;
    if (Bot.BodyType == EBodyType::Quinn)
    {
        Material = (Bot.OutfitVariant == 0) ? QuinnMaterialA : QuinnMaterialB;
    }
    else
    {
        Material = (Bot.OutfitVariant == 0) ? MannyMaterialA : MannyMaterialB;
    }

    if (MeshAsset)
    {
        Bot.Mesh->SetSkeletalMesh(MeshAsset);
    }
    if (Material)
    {
        for (int32 MaterialIndex = 0; MaterialIndex < Bot.Mesh->GetNumMaterials(); ++MaterialIndex)
        {
            Bot.Mesh->SetMaterial(MaterialIndex, Material);
        }
    }

    Bot.Mesh->SetAnimationMode(EAnimationMode::AnimationSingleNode);
}

void UConventionBotSubsystem::InitializeBot(FConventionBotState& Bot)
{
    if (!Bot.Actor.IsValid() || !Bot.Mesh.IsValid() || !Bot.LocalCharacter.IsValid())
    {
        return;
    }

    const FString VoiceId = ReadStringTag(Bot.Actor.Get(), TEXT("CB_Voice="), TEXT(""));
    if (!VoiceId.IsEmpty())
    {
        Bot.LocalCharacter->VoiceId = FName(*VoiceId);
    }

    Bot.PatrolOrigin = Bot.Actor->GetActorLocation();
    Bot.BaseActorRotation = Bot.Actor->GetActorRotation();
    Bot.MeshBaseRelativeLocation = Bot.Mesh->GetRelativeLocation();
    Bot.MeshBaseRelativeRotation = Bot.Mesh->GetRelativeRotation();
    ApplyVisuals(Bot);
    SnapToGround(Bot, 1.0f);
    Bot.bInitialized = true;
}

void UConventionBotSubsystem::UpdateRoaming(FConventionBotState& Bot, float DeltaTime)
{
    Bot.bMovingLastTick = false;

    if (!Bot.bRoam || Bot.PatrolOffsets.Num() == 0 || IsTalking(Bot) || !Bot.Actor.IsValid())
    {
        return;
    }

    if (Bot.PauseRemaining > 0.0f)
    {
        Bot.PauseRemaining = FMath::Max(0.0f, Bot.PauseRemaining - DeltaTime);
        return;
    }

    const int32 SafeIndex = FMath::Clamp(Bot.PatrolIndex, 0, Bot.PatrolOffsets.Num() - 1);
    FVector Target = Bot.PatrolOrigin + Bot.PatrolOffsets[SafeIndex];
    FVector ToTarget = Target - Bot.Actor->GetActorLocation();
    ToTarget.Z = 0.0f;

    const float Distance = ToTarget.Size();
    if (Distance <= 70.0f)
    {
        Bot.PauseRemaining = 1.2f;
        Bot.PatrolIndex = (Bot.PatrolIndex + 1) % Bot.PatrolOffsets.Num();
        return;
    }

    const FVector Direction = ToTarget.GetSafeNormal();
    const float DistanceAlpha = FMath::Clamp(Distance / 260.0f, 0.35f, 1.0f);
    const float StepDistance = FMath::Min(Distance, Bot.RoamSpeed * DistanceAlpha * DeltaTime);
    const FVector DeltaMove = Direction * StepDistance;
    const FVector StartLocation = Bot.Actor->GetActorLocation();

    FHitResult SweepHit;
    Bot.Actor->AddActorWorldOffset(DeltaMove, true, &SweepHit);
    FVector HorizontalMove = Bot.Actor->GetActorLocation() - StartLocation;
    HorizontalMove.Z = 0.0f;
    Bot.bMovingLastTick = HorizontalMove.SizeSquared() > 4.0f;

    if (SweepHit.bBlockingHit && !Bot.bMovingLastTick)
    {
        Bot.PauseRemaining = 0.75f;
        Bot.PatrolIndex = (Bot.PatrolIndex + 1) % Bot.PatrolOffsets.Num();
        return;
    }

    if (Bot.bMovingLastTick)
    {
        const float DesiredYaw = Direction.Rotation().Yaw;
        const float NewYaw = FMath::FixedTurn(Bot.Actor->GetActorRotation().Yaw, DesiredYaw, 360.0f * DeltaTime);
        Bot.Actor->SetActorRotation(MakeActorYawRotation(Bot, NewYaw));
    }
}

void UConventionBotSubsystem::UpdateFacing(FConventionBotState& Bot, float DeltaTime)
{
    if (!Bot.Actor.IsValid() || !IsTalking(Bot))
    {
        return;
    }

    AActor* Target = FindFacingTarget(Bot);
    if (!Target)
    {
        return;
    }

    FVector ToTarget = Target->GetActorLocation() - Bot.Actor->GetActorLocation();
    ToTarget.Z = 0.0f;
    if (ToTarget.IsNearlyZero())
    {
        return;
    }

    const float DesiredYaw = ToTarget.Rotation().Yaw;
    const float NewYaw = FMath::FixedTurn(Bot.Actor->GetActorRotation().Yaw, DesiredYaw, 300.0f * DeltaTime);
    Bot.Actor->SetActorRotation(MakeActorYawRotation(Bot, NewYaw));
}

void UConventionBotSubsystem::UpdateAnimation(FConventionBotState& Bot, float DeltaTime)
{
    if (!Bot.Mesh.IsValid())
    {
        return;
    }

    UAnimationAsset* DesiredAnimation = nullptr;
    float DesiredRate = 1.0f;
    const bool bTalking = IsTalking(Bot);

    if (Bot.BodyType == EBodyType::Quinn)
    {
        if (bTalking)
        {
            DesiredAnimation = QuinnTalkAnimation;
            DesiredRate = FMath::Max(0.9f, Bot.TalkAnimRate + 0.55f);
        }
        else if (Bot.bMovingLastTick)
        {
            DesiredAnimation = QuinnWalkAnimation;
            DesiredRate = Bot.WalkAnimRate;
        }
        else
        {
            DesiredAnimation = QuinnIdleAnimation;
        }
    }
    else
    {
        if (bTalking)
        {
            DesiredAnimation = MannyTalkAnimation;
            DesiredRate = FMath::Max(0.9f, Bot.TalkAnimRate + 0.55f);
        }
        else if (Bot.bMovingLastTick)
        {
            DesiredAnimation = MannyWalkAnimation;
            DesiredRate = Bot.WalkAnimRate;
        }
        else
        {
            DesiredAnimation = MannyIdleAnimation;
        }
    }

    if (!DesiredAnimation)
    {
        return;
    }

    if (Bot.ActiveAnimation != DesiredAnimation)
    {
        Bot.Mesh->PlayAnimation(DesiredAnimation, true);
        Bot.ActiveAnimation = DesiredAnimation;
    }
    Bot.Mesh->SetPlayRate(DesiredRate);

    if (bTalking)
    {
        Bot.TalkPoseTime += DeltaTime;

        const float SwayYaw = FMath::Sin(Bot.TalkPoseTime * 5.4f) * 4.0f;
        const float SwayRoll = FMath::Sin(Bot.TalkPoseTime * 10.8f) * 1.2f;
        const float BobZ = FMath::Sin(Bot.TalkPoseTime * 10.8f) * 1.8f;

        Bot.Mesh->SetRelativeLocation(Bot.MeshBaseRelativeLocation + FVector(0.0f, 0.0f, BobZ));
        Bot.Mesh->SetRelativeRotation(Bot.MeshBaseRelativeRotation + FRotator(0.0f, SwayYaw, SwayRoll));
        return;
    }

    Bot.TalkPoseTime = 0.0f;
    Bot.Mesh->SetRelativeLocation(FMath::VInterpTo(Bot.Mesh->GetRelativeLocation(), Bot.MeshBaseRelativeLocation, DeltaTime, 10.0f));
    Bot.Mesh->SetRelativeRotation(FMath::RInterpTo(Bot.Mesh->GetRelativeRotation(), Bot.MeshBaseRelativeRotation, DeltaTime, 10.0f));
}

void UConventionBotSubsystem::SnapToGround(FConventionBotState& Bot, float DeltaTime)
{
    if (!Bot.Actor.IsValid())
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    const FVector ActorLocation = Bot.Actor->GetActorLocation();
    const FVector TraceStart = ActorLocation + FVector(0.0f, 0.0f, 250.0f);
    const FVector TraceEnd = ActorLocation - FVector(0.0f, 0.0f, 1000.0f);

    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(ConventionBotGroundSnap), false, Bot.Actor.Get());
    FHitResult Hit;
    if (!World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECollisionChannel::ECC_Visibility, QueryParams))
    {
        return;
    }

    if (!Bot.bHasGroundOffset)
    {
        Bot.GroundOffsetZ = ActorLocation.Z - Hit.Location.Z;
        Bot.bHasGroundOffset = true;
    }

    const float TargetZ = Hit.Location.Z + Bot.GroundOffsetZ;
    const float NewZ = FMath::FInterpTo(ActorLocation.Z, TargetZ, DeltaTime, 12.0f);
    if (!FMath::IsNearlyEqual(NewZ, ActorLocation.Z, 0.5f))
    {
        Bot.Actor->SetActorLocation(FVector(ActorLocation.X, ActorLocation.Y, NewZ), false);
    }
}

bool UConventionBotSubsystem::IsTalking(const FConventionBotState& Bot) const
{
    return Bot.LocalCharacter.IsValid() && (Bot.LocalCharacter->IsBusy() || Bot.LocalCharacter->IsAudioPlaying());
}

AActor* UConventionBotSubsystem::FindFacingTarget(const FConventionBotState& Bot) const
{
    if (!Bot.Actor.IsValid())
    {
        return nullptr;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return nullptr;
    }

    APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(World, 0);
    if (PlayerPawn)
    {
        const float PlayerDistanceSq = FVector::DistSquared2D(PlayerPawn->GetActorLocation(), Bot.Actor->GetActorLocation());
        if (PlayerDistanceSq <= FMath::Square(FMath::Max(0.0f, Bot.FacePlayerRadius)))
        {
            return PlayerPawn;
        }
    }

    AActor* BestBot = nullptr;
    AActor* BestTalkingBot = nullptr;
    float BestDistanceSq = FMath::Square(FMath::Max(0.0f, Bot.FaceBotRadius));
    float BestTalkingDistanceSq = BestDistanceSq;
    for (const FConventionBotState& OtherBot : Bots)
    {
        if (!OtherBot.Actor.IsValid() || OtherBot.Actor == Bot.Actor)
        {
            continue;
        }

        const float DistanceSq = FVector::DistSquared2D(OtherBot.Actor->GetActorLocation(), Bot.Actor->GetActorLocation());
        if (DistanceSq <= BestDistanceSq)
        {
            BestDistanceSq = DistanceSq;
            BestBot = OtherBot.Actor.Get();
        }

        if (IsTalking(OtherBot) && DistanceSq <= BestTalkingDistanceSq)
        {
            BestTalkingDistanceSq = DistanceSq;
            BestTalkingBot = OtherBot.Actor.Get();
        }
    }

    return BestTalkingBot ? BestTalkingBot : BestBot;
}

FRotator UConventionBotSubsystem::MakeActorYawRotation(const FConventionBotState& Bot, float Yaw) const
{
    return FRotator(Bot.BaseActorRotation.Pitch, Yaw, Bot.BaseActorRotation.Roll);
}

bool UConventionBotSubsystem::ReadBoolTag(const AActor* Actor, const FString& Prefix, bool DefaultValue) const
{
    const FString Value = ReadStringTag(Actor, Prefix, DefaultValue ? TEXT("1") : TEXT("0"));
    return Value.Equals(TEXT("1")) || Value.Equals(TEXT("true"), ESearchCase::IgnoreCase);
}

float UConventionBotSubsystem::ReadFloatTag(const AActor* Actor, const FString& Prefix, float DefaultValue) const
{
    const FString Value = ReadStringTag(Actor, Prefix, FString::SanitizeFloat(DefaultValue));
    return FCString::Atof(*Value);
}

int32 UConventionBotSubsystem::ReadIntTag(const AActor* Actor, const FString& Prefix, int32 DefaultValue) const
{
    const FString Value = ReadStringTag(Actor, Prefix, FString::FromInt(DefaultValue));
    return FCString::Atoi(*Value);
}

FString UConventionBotSubsystem::ReadStringTag(const AActor* Actor, const FString& Prefix, const FString& DefaultValue) const
{
    if (!Actor)
    {
        return DefaultValue;
    }

    for (const FName& Tag : Actor->Tags)
    {
        const FString TagValue = Tag.ToString();
        if (TagValue.StartsWith(Prefix))
        {
            return TagValue.RightChop(Prefix.Len());
        }
    }
    return DefaultValue;
}

TArray<FVector> UConventionBotSubsystem::ReadPatrolOffsets(const AActor* Actor) const
{
    TArray<FVector> Result;
    const FString Raw = ReadStringTag(Actor, TEXT("CB_Patrol="), TEXT(""));
    if (Raw.IsEmpty())
    {
        return Result;
    }

    TArray<FString> PointStrings;
    Raw.ParseIntoArray(PointStrings, TEXT(";"), true);
    for (const FString& PointString : PointStrings)
    {
        TArray<FString> Parts;
        PointString.ParseIntoArray(Parts, TEXT(","), true);
        if (Parts.Num() != 3)
        {
            continue;
        }

        Result.Add(FVector(
            FCString::Atof(*Parts[0]),
            FCString::Atof(*Parts[1]),
            FCString::Atof(*Parts[2])));
    }

    return Result;
}
