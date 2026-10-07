#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "LocalTalkerSubsystem.generated.h"
class IWebSocket;
class ULocalTalkerCharacterComponent;
namespace Audio { class FAudioCapture; }

USTRUCT(BlueprintType)
struct FLocalTalkerLine {
 GENERATED_BODY()
 UPROPERTY(BlueprintReadOnly, Category="PersonalityCore") FString Speaker;
 UPROPERTY(BlueprintReadOnly, Category="PersonalityCore") FString Name;
 UPROPERTY(BlueprintReadOnly, Category="PersonalityCore") FString Text;
};
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FLocalTalkerEvent, const FString&, EventJson);

/** Thin REST/WebSocket client. Scene policy and dialogue direction live in the shared runtime. */
UCLASS()
class LOCALTALKER_API ULocalTalkerSubsystem : public UGameInstanceSubsystem, public FTickableGameObject {
 GENERATED_BODY()
public:
 virtual void Initialize(FSubsystemCollectionBase& Collection) override;
 virtual void Deinitialize() override;
 virtual void Tick(float DeltaTime) override;
 virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(ULocalTalkerSubsystem, STATGROUP_Tickables); }
 virtual bool IsTickable() const override { return !IsTemplate(); }
 UFUNCTION(BlueprintCallable, Category="PersonalityCore") void OpenSceneJson(const FString& DefinitionJson);
 UFUNCTION(BlueprintCallable, Category="PersonalityCore") void ConnectScene(const FString& SceneId);
 UFUNCTION(BlueprintCallable, Category="PersonalityCore") void SendText(const FString& Text, const FString& Target=TEXT("all"));
 UFUNCTION(BlueprintCallable, Category="PersonalityCore") void SetPlayerPresence(bool bPresent, const FString& PlayerName=TEXT("Engineer"));
 UFUNCTION(BlueprintCallable, Category="PersonalityCore") void JoinConversation();
 UFUNCTION(BlueprintCallable, Category="PersonalityCore") void BeginPushToTalk();
 UFUNCTION(BlueprintCallable, Category="PersonalityCore") void EndPushToTalk(const FString& Target=TEXT("all"));
 UFUNCTION(BlueprintCallable, Category="PersonalityCore") void Interrupt();
 UFUNCTION(BlueprintCallable, Category="PersonalityCore") void ContinueConversation(int32 Turns=6);
 /** Ask one character for a single unprompted turn, with a private direction only they see. */
 UFUNCTION(BlueprintCallable, Category="PersonalityCore") void PromptCharacter(const FString& Key, const FString& Reason);
 UFUNCTION(BlueprintCallable, Category="PersonalityCore") void SetDirection(const FString& Goal, const FString& Guidance, const FString& JoinPolicy=TEXT("open"));
 UFUNCTION(BlueprintCallable, Category="PersonalityCore") void SendCommandJson(const FString& Json);
 UFUNCTION(BlueprintCallable, Category="PersonalityCore") void SubmitPCM16(const TArray<uint8>& PCM, int32 SampleRate, const FString& Target=TEXT("all"));
 void RegisterCharacter(ULocalTalkerCharacterComponent* Character);
 void UnregisterCharacter(ULocalTalkerCharacterComponent* Character);
 UPROPERTY(BlueprintAssignable, Category="PersonalityCore") FLocalTalkerEvent OnSceneEvent;
 UPROPERTY(BlueprintReadOnly, Category="PersonalityCore") bool bConnected=false;
 UPROPERTY(BlueprintReadOnly, Category="PersonalityCore") bool bJoined=false;
 UPROPERTY(BlueprintReadOnly, Category="PersonalityCore") bool bRecording=false;
 UPROPERTY(BlueprintReadOnly, Category="PersonalityCore") float MicrophoneLevel=0;
 UPROPERTY(BlueprintReadOnly, Category="PersonalityCore") FString State=TEXT("Connecting");
 UPROPERTY(BlueprintReadOnly, Category="PersonalityCore") FString Error;
 UPROPERTY(BlueprintReadOnly, Category="PersonalityCore") FString ActiveSpeaker;
 UPROPERTY(BlueprintReadOnly, Category="PersonalityCore") FString ActiveName;
 UPROPERTY(BlueprintReadOnly, Category="PersonalityCore") FString StreamingText;
 UPROPERTY(BlueprintReadOnly, Category="PersonalityCore") FString Goal;
 UPROPERTY(BlueprintReadOnly, Category="PersonalityCore") FString JoinPolicy=TEXT("open");
 UPROPERTY(BlueprintReadOnly, Category="PersonalityCore") FString LastStructuredReply;
 UPROPERTY(BlueprintReadOnly, Category="PersonalityCore") TArray<FLocalTalkerLine> History;
 UPROPERTY(BlueprintReadOnly, Category="PersonalityCore") FString ConnectedSceneId;
 UPROPERTY(BlueprintReadOnly, Category="PersonalityCore|Diagnostics") int64 ReceivedAudioBytes=0;
 UPROPERTY(BlueprintReadOnly, Category="PersonalityCore|Diagnostics") int64 CapturedAudioBytes=0;
 UPROPERTY(BlueprintReadOnly, Category="PersonalityCore|Diagnostics") int32 CompletedPlaybackTurns=0;
private:
 FString BaseUrl;
 FString CurrentTurn;
 FString LastDefinition;
 TSharedPtr<IWebSocket> Socket;
 TSharedPtr<Audio::FAudioCapture> Capture;
 FCriticalSection CaptureMutex;
 TArray<uint8> CapturedPCM;
 int32 CaptureRate=48000;
 double AudioEndsAt=0;
 double RecordingStartedAt=0;
 double RetryAt=0;
 bool bAwaitingPlayback=false;
 bool bShuttingDown=false;
 TArray<TWeakObjectPtr<ULocalTalkerCharacterComponent>> Characters;
 void Receive(const FString& Json);
 void StopPlayback();
 void SimpleCommand(const FString& Type);
};
