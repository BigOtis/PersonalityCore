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
 UPROPERTY(BlueprintReadOnly) FString Speaker;
 UPROPERTY(BlueprintReadOnly) FString Name;
 UPROPERTY(BlueprintReadOnly) FString Text;
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
 UFUNCTION(BlueprintCallable, Category="LocalTalker") void OpenSceneJson(const FString& DefinitionJson);
 UFUNCTION(BlueprintCallable, Category="LocalTalker") void ConnectScene(const FString& SceneId);
 UFUNCTION(BlueprintCallable, Category="LocalTalker") void SendText(const FString& Text, const FString& Target=TEXT("all"));
 UFUNCTION(BlueprintCallable, Category="LocalTalker") void SetPlayerPresence(bool bPresent, const FString& PlayerName=TEXT("Engineer"));
 UFUNCTION(BlueprintCallable, Category="LocalTalker") void JoinConversation();
 UFUNCTION(BlueprintCallable, Category="LocalTalker") void BeginPushToTalk();
 UFUNCTION(BlueprintCallable, Category="LocalTalker") void EndPushToTalk(const FString& Target=TEXT("all"));
 UFUNCTION(BlueprintCallable, Category="LocalTalker") void Interrupt();
 UFUNCTION(BlueprintCallable, Category="LocalTalker") void ContinueConversation(int32 Turns=6);
 /** Ask one character for a single unprompted turn, with a private direction only they see. */
 UFUNCTION(BlueprintCallable, Category="LocalTalker") void PromptCharacter(const FString& Key, const FString& Reason);
 UFUNCTION(BlueprintCallable, Category="LocalTalker") void SetDirection(const FString& Goal, const FString& Guidance, const FString& JoinPolicy=TEXT("open"));
 UFUNCTION(BlueprintCallable, Category="LocalTalker") void SendCommandJson(const FString& Json);
 UFUNCTION(BlueprintCallable, Category="LocalTalker") void SubmitPCM16(const TArray<uint8>& PCM, int32 SampleRate, const FString& Target=TEXT("all"));
 void RegisterCharacter(ULocalTalkerCharacterComponent* Character);
 void UnregisterCharacter(ULocalTalkerCharacterComponent* Character);
 UPROPERTY(BlueprintAssignable, Category="LocalTalker") FLocalTalkerEvent OnSceneEvent;
 UPROPERTY(BlueprintReadOnly, Category="LocalTalker") bool bConnected=false;
 UPROPERTY(BlueprintReadOnly, Category="LocalTalker") bool bJoined=false;
 UPROPERTY(BlueprintReadOnly, Category="LocalTalker") bool bRecording=false;
 UPROPERTY(BlueprintReadOnly, Category="LocalTalker") float MicrophoneLevel=0;
 UPROPERTY(BlueprintReadOnly, Category="LocalTalker") FString State=TEXT("Connecting");
 UPROPERTY(BlueprintReadOnly, Category="LocalTalker") FString Error;
 UPROPERTY(BlueprintReadOnly, Category="LocalTalker") FString ActiveSpeaker;
 UPROPERTY(BlueprintReadOnly, Category="LocalTalker") FString ActiveName;
 UPROPERTY(BlueprintReadOnly, Category="LocalTalker") FString StreamingText;
 UPROPERTY(BlueprintReadOnly, Category="LocalTalker") FString Goal;
 UPROPERTY(BlueprintReadOnly, Category="LocalTalker") FString JoinPolicy=TEXT("open");
 UPROPERTY(BlueprintReadOnly, Category="LocalTalker") FString LastStructuredReply;
 UPROPERTY(BlueprintReadOnly, Category="LocalTalker") TArray<FLocalTalkerLine> History;
 UPROPERTY(BlueprintReadOnly, Category="LocalTalker") FString ConnectedSceneId;
 UPROPERTY(BlueprintReadOnly, Category="LocalTalker|Diagnostics") int64 ReceivedAudioBytes=0;
 UPROPERTY(BlueprintReadOnly, Category="LocalTalker|Diagnostics") int64 CapturedAudioBytes=0;
 UPROPERTY(BlueprintReadOnly, Category="LocalTalker|Diagnostics") int32 CompletedPlaybackTurns=0;
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
