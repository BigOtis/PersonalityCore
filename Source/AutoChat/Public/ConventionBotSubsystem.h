#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Tickable.h"
#include "ConventionBotSubsystem.generated.h"

class UAnimationAsset;
class ULocalCharacterComponent;
class UMaterialInterface;
class USkeletalMesh;
class USkeletalMeshComponent;

UCLASS()
class AUTOCHAT_API UConventionBotSubsystem : public UTickableWorldSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;

private:
    enum class EBodyType : uint8
    {
        Manny,
        Quinn
    };

    struct FConventionBotState
    {
        TWeakObjectPtr<AActor> Actor;
        TWeakObjectPtr<ULocalCharacterComponent> LocalCharacter;
        TWeakObjectPtr<USkeletalMeshComponent> Mesh;
        TArray<FVector> PatrolOffsets;
        FVector PatrolOrigin = FVector::ZeroVector;
        FVector MeshBaseRelativeLocation = FVector::ZeroVector;
        FRotator BaseActorRotation = FRotator::ZeroRotator;
        FRotator MeshBaseRelativeRotation = FRotator::ZeroRotator;
        int32 PatrolIndex = 0;
        int32 OutfitVariant = 0;
        bool bInitialized = false;
        bool bHasGroundOffset = false;
        bool bRoam = false;
        bool bMovingLastTick = false;
        float PauseRemaining = 0.0f;
        float RoamSpeed = 150.0f;
        float FacePlayerRadius = 850.0f;
        float FaceBotRadius = 900.0f;
        float GroundOffsetZ = 0.0f;
        float TalkAnimRate = 0.35f;
        float TalkPoseTime = 0.0f;
        float WalkAnimRate = 1.0f;
        EBodyType BodyType = EBodyType::Manny;
        TObjectPtr<UAnimationAsset> ActiveAnimation = nullptr;
    };

    void EnsureAssetsLoaded();
    void RefreshBots(bool bForce = false);
    void ApplyVisuals(FConventionBotState& Bot);
    void InitializeBot(FConventionBotState& Bot);
    void UpdateRoaming(FConventionBotState& Bot, float DeltaTime);
    void UpdateFacing(FConventionBotState& Bot, float DeltaTime);
    void UpdateAnimation(FConventionBotState& Bot, float DeltaTime);
    void SnapToGround(FConventionBotState& Bot, float DeltaTime);
    bool IsTalking(const FConventionBotState& Bot) const;
    AActor* FindFacingTarget(const FConventionBotState& Bot) const;
    FRotator MakeActorYawRotation(const FConventionBotState& Bot, float Yaw) const;

    bool ReadBoolTag(const AActor* Actor, const FString& Prefix, bool DefaultValue) const;
    float ReadFloatTag(const AActor* Actor, const FString& Prefix, float DefaultValue) const;
    int32 ReadIntTag(const AActor* Actor, const FString& Prefix, int32 DefaultValue) const;
    FString ReadStringTag(const AActor* Actor, const FString& Prefix, const FString& DefaultValue) const;
    TArray<FVector> ReadPatrolOffsets(const AActor* Actor) const;

    TObjectPtr<USkeletalMesh> MannyMesh = nullptr;
    TObjectPtr<USkeletalMesh> QuinnMesh = nullptr;
    TObjectPtr<UAnimationAsset> MannyIdleAnimation = nullptr;
    TObjectPtr<UAnimationAsset> MannyWalkAnimation = nullptr;
    TObjectPtr<UAnimationAsset> MannyTalkAnimation = nullptr;
    TObjectPtr<UAnimationAsset> QuinnIdleAnimation = nullptr;
    TObjectPtr<UAnimationAsset> QuinnWalkAnimation = nullptr;
    TObjectPtr<UAnimationAsset> QuinnTalkAnimation = nullptr;
    TObjectPtr<UMaterialInterface> MannyMaterialA = nullptr;
    TObjectPtr<UMaterialInterface> MannyMaterialB = nullptr;
    TObjectPtr<UMaterialInterface> QuinnMaterialA = nullptr;
    TObjectPtr<UMaterialInterface> QuinnMaterialB = nullptr;

    TArray<FConventionBotState> Bots;
    double NextRefreshTimeSeconds = 0.0;
};
