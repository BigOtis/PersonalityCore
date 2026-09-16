#include "LocalTalkerSubsystem.h"
#include "LocalTalkerCharacterComponent.h"
#include "LocalTalkerSettings.h"
#include "AudioCaptureCore.h"
#include "WebSocketsModule.h"
#include "IWebSocket.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/Base64.h"
#include "Misc/ScopeLock.h"

namespace {
FString Encode(const TSharedRef<FJsonObject>& Object) { FString Text; FJsonSerializer::Serialize(Object,TJsonWriterFactory<>::Create(&Text)); return Text; }
TSharedPtr<FJsonObject> Decode(const FString& Text) { TSharedPtr<FJsonObject> Object; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Object); return Object; }
FString String(const TSharedPtr<FJsonObject>& O,const TCHAR* Key) { FString V; if (O) O->TryGetStringField(Key,V); return V; }
}

void ULocalTalkerSubsystem::Initialize(FSubsystemCollectionBase& Collection) {
 Super::Initialize(Collection); BaseUrl=GetDefault<ULocalTalkerSettings>()->RuntimeUrl; BaseUrl.RemoveFromEnd(TEXT("/"));
 Capture=MakeShared<Audio::FAudioCapture>();
}
void ULocalTalkerSubsystem::Deinitialize() {
 bShuttingDown=true;
 if (Capture) { Capture->StopStream(); Capture->CloseStream(); }
 bRecording=false;
 if (Socket) { Socket->OnClosed().Clear(); Socket->OnMessage().Clear(); Socket->OnConnected().Clear(); Socket->OnConnectionError().Clear(); Socket->Close(); Socket.Reset(); }
 StopPlayback(); Capture.Reset(); Super::Deinitialize();
}
void ULocalTalkerSubsystem::RegisterCharacter(ULocalTalkerCharacterComponent* C) { Characters.AddUnique(C); }
void ULocalTalkerSubsystem::UnregisterCharacter(ULocalTalkerCharacterComponent* C) { Characters.Remove(C); }
void ULocalTalkerSubsystem::StopPlayback() {
 for (auto C:Characters) if(C.IsValid()) C->StopVoice();
 AudioEndsAt=0; bAwaitingPlayback=false; StreamingText.Empty();
}
void ULocalTalkerSubsystem::OpenSceneJson(const FString& DefinitionJson) {
 LastDefinition=DefinitionJson; Error.Empty(); State=TEXT("Connecting"); RetryAt=0;
 auto Definition=Decode(DefinitionJson);
 if (!Definition) {Error=TEXT("Scene file is not valid JSON");return;}
 const FString Name=String(Definition,TEXT("name"));
 auto Request=FHttpModule::Get().CreateRequest();
 Request->SetURL(BaseUrl+TEXT("/v1/scenes")); Request->SetVerb(TEXT("GET")); Request->SetTimeout(5);
 TWeakObjectPtr<ULocalTalkerSubsystem> Weak(this);
 Request->OnProcessRequestComplete().BindLambda([Weak,DefinitionJson,Name](FHttpRequestPtr, FHttpResponsePtr Response,bool Success) {
  if(!Weak.IsValid() || Weak->bShuttingDown)return;
  if(!Success || !Response.IsValid() || Response->GetResponseCode()!=200) {
   Weak->Error=TEXT("LocalTalker runtime unavailable. Start LocalTalker or its runtime service. Retrying..."); Weak->RetryAt=FPlatformTime::Seconds()+3; return;
  }
  TArray<TSharedPtr<FJsonValue>> Scenes;
  if(FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response->GetContentAsString()),Scenes)) {
   for(auto V:Scenes) {auto O=V->AsObject(); if(O.IsValid() && O->HasTypedField<EJson::Object>(TEXT("definition")) && String(O->GetObjectField(TEXT("definition")),TEXT("name"))==Name) {Weak->ConnectScene(String(O,TEXT("id")));return;}}
  }
  auto Create=FHttpModule::Get().CreateRequest(); Create->SetURL(Weak->BaseUrl+TEXT("/v1/scenes")); Create->SetVerb(TEXT("POST")); Create->SetHeader(TEXT("Content-Type"),TEXT("application/json")); Create->SetContentAsString(DefinitionJson);
  Create->OnProcessRequestComplete().BindLambda([Weak](FHttpRequestPtr,FHttpResponsePtr R,bool OK){
   if(!Weak.IsValid())return;
   if(OK && R.IsValid() && R->GetResponseCode()==201) { auto O=Decode(R->GetContentAsString()); Weak->ConnectScene(String(O,TEXT("id"))); }
   else {Weak->Error=R.IsValid()?R->GetContentAsString():TEXT("Could not create scene");Weak->State=TEXT("Error");}
  }); Create->ProcessRequest();
 }); Request->ProcessRequest();
}
void ULocalTalkerSubsystem::ConnectScene(const FString& SceneId) {
 if(Socket) {Socket->OnClosed().Clear();Socket->OnMessage().Clear();Socket->OnConnectionError().Clear();Socket->OnConnected().Clear();Socket->Close();}
 ConnectedSceneId=SceneId; bConnected=false; StopPlayback();
 FString URL=BaseUrl; URL.ReplaceInline(TEXT("https://"),TEXT("wss://"));URL.ReplaceInline(TEXT("http://"),TEXT("ws://"));
 TMap<FString,FString> Headers;Headers.Add(TEXT("X-LocalTalker-Client"),TEXT("unreal"));
 Socket=FWebSocketsModule::Get().CreateWebSocket(URL+TEXT("/v1/scenes/")+SceneId+TEXT("/live"),FString(),Headers);
 Socket->SetTextMessageMemoryLimit(16000000);
 Socket->OnConnected().AddWeakLambda(this,[this](){bConnected=true;Error.Empty();State=TEXT("Ready");RetryAt=0;});
 Socket->OnMessage().AddUObject(this,&ULocalTalkerSubsystem::Receive);
 Socket->OnConnectionError().AddWeakLambda(this,[this](const FString& Message){Error=Message;bConnected=false;RetryAt=FPlatformTime::Seconds()+3;});
 Socket->OnClosed().AddWeakLambda(this,[this](int32,const FString&,bool){bConnected=false;bJoined=false;StopPlayback();State=TEXT("Reconnecting");RetryAt=FPlatformTime::Seconds()+3;});
 Socket->Connect();
}
void ULocalTalkerSubsystem::SendCommandJson(const FString& Json) { if(Socket && Socket->IsConnected())Socket->Send(Json);else Error=TEXT("Runtime disconnected; reconnecting automatically."); }
void ULocalTalkerSubsystem::SimpleCommand(const FString& Type) {auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("type"),Type);SendCommandJson(Encode(O));}
void ULocalTalkerSubsystem::SendText(const FString& Text,const FString& Target) {if(Text.TrimStartAndEnd().IsEmpty())return;Interrupt();auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("type"),TEXT("text"));O->SetStringField(TEXT("text"),Text);O->SetStringField(TEXT("target"),Target);SendCommandJson(Encode(O));}
void ULocalTalkerSubsystem::SetPlayerPresence(bool Present,const FString& Name) {auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("type"),TEXT("presence"));O->SetBoolField(TEXT("present"),Present);O->SetStringField(TEXT("player_name"),Name);SendCommandJson(Encode(O));}
void ULocalTalkerSubsystem::JoinConversation(){SimpleCommand(TEXT("join"));}
void ULocalTalkerSubsystem::Interrupt(){
 if(bRecording){Capture->StopStream();Capture->CloseStream();FScopeLock Guard(&CaptureMutex);bRecording=false;CapturedPCM.Empty();MicrophoneLevel=0;}
 StopPlayback();CurrentTurn.Empty();ActiveSpeaker.Empty();State=TEXT("Ready");SimpleCommand(TEXT("interrupt"));
}
void ULocalTalkerSubsystem::ContinueConversation(int32 Turns){auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("type"),TEXT("continue"));O->SetNumberField(TEXT("turns"),FMath::Clamp(Turns,1,30));SendCommandJson(Encode(O));}
void ULocalTalkerSubsystem::PromptCharacter(const FString& Key,const FString& Reason){
 if(Key.IsEmpty())return;
 auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("type"),TEXT("continue"));O->SetStringField(TEXT("target"),Key);O->SetStringField(TEXT("text"),Reason);O->SetNumberField(TEXT("turns"),1);SendCommandJson(Encode(O));
}
void ULocalTalkerSubsystem::SetDirection(const FString& NewGoal,const FString& Guidance,const FString& Policy){auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("type"),TEXT("direct"));O->SetStringField(TEXT("goal"),NewGoal);O->SetStringField(TEXT("guidance"),Guidance);O->SetStringField(TEXT("join_policy"),Policy);SendCommandJson(Encode(O));}
void ULocalTalkerSubsystem::BeginPushToTalk(){
 if(!bConnected || bRecording)return;
 if(!bJoined){Error=TEXT("Approach the droids and join their conversation first.");return;}
 Interrupt();
 {FScopeLock Guard(&CaptureMutex);CapturedPCM.Empty();MicrophoneLevel=0;}
 Audio::FAudioCaptureDeviceParams Params;Params.DeviceIndex=GetDefault<ULocalTalkerSettings>()->CaptureDeviceIndex;Params.bUseHardwareAEC=true;
 const bool Open=Capture->OpenAudioCaptureStream(Params,[this](const void* Data,int32 Frames,int32 Channels,int32 Rate,double,bool){
  FScopeLock Guard(&CaptureMutex);if(!bRecording)return;
  const float* Input=static_cast<const float*>(Data); CaptureRate=Rate;float Energy=0;
  for(int32 F=0;F<Frames;++F){float Value=0;for(int32 C=0;C<Channels;++C)Value+=Input[F*Channels+C];Value=FMath::Clamp(Value/Channels,-1.f,1.f);Energy+=Value*Value;const int16 Sample=static_cast<int16>(Value*32767);CapturedPCM.Append(reinterpret_cast<const uint8*>(&Sample),2);}
  CapturedAudioBytes=CapturedPCM.Num();MicrophoneLevel=FMath::Sqrt(Energy/FMath::Max(Frames,1));
 },512);
 if(!Open){Error=TEXT("Microphone unavailable. Check Windows microphone permissions or use Enter for text.");return;}
 {FScopeLock Guard(&CaptureMutex);bRecording=true;}
 if(!Capture->StartStream()){bRecording=false;Capture->CloseStream();Error=TEXT("Microphone could not start");return;}
 RecordingStartedAt=FPlatformTime::Seconds(); State=TEXT("Listening");Error.Empty();
}
void ULocalTalkerSubsystem::EndPushToTalk(const FString& Target){
 if(!bRecording)return;Capture->StopStream();Capture->CloseStream();TArray<uint8> PCM;
 {FScopeLock Guard(&CaptureMutex);bRecording=false;PCM=MoveTemp(CapturedPCM);MicrophoneLevel=0;}
 SubmitPCM16(PCM,CaptureRate,Target);
}
void ULocalTalkerSubsystem::SubmitPCM16(const TArray<uint8>& PCM,int32 Rate,const FString& Target){
 if(PCM.IsEmpty()){State=TEXT("Ready");return;}
 auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("type"),TEXT("audio"));O->SetStringField(TEXT("pcm16_b64"),FBase64::Encode(PCM));O->SetNumberField(TEXT("sample_rate"),Rate);O->SetNumberField(TEXT("channels"),1);SendCommandJson(Encode(O));
 O=MakeShared<FJsonObject>();O->SetStringField(TEXT("type"),TEXT("end_audio"));O->SetStringField(TEXT("target"),Target);SendCommandJson(Encode(O));State=TEXT("Transcribing");
}
void ULocalTalkerSubsystem::Tick(float){
 const double Now=FPlatformTime::Seconds();
 if(bShuttingDown)return;
 if(RetryAt>0 && Now>RetryAt){RetryAt=0;if(!ConnectedSceneId.IsEmpty())ConnectScene(ConnectedSceneId);else if(!LastDefinition.IsEmpty())OpenSceneJson(LastDefinition);}
 if(bRecording && Now-RecordingStartedAt>60)EndPushToTalk();
 if(bAwaitingPlayback && Now>=AudioEndsAt+0.15){
  bAwaitingPlayback=false;++CompletedPlaybackTurns;auto O=MakeShared<FJsonObject>();O->SetStringField(TEXT("type"),TEXT("playback_done"));O->SetStringField(TEXT("turn_id"),CurrentTurn);SendCommandJson(Encode(O));
  for(auto C:Characters)if(C.IsValid())C->StopVoice();State=TEXT("Ready");
 }
}
void ULocalTalkerSubsystem::Receive(const FString& Json){
 auto O=Decode(Json);if(!O)return;const FString Type=String(O,TEXT("type"));
 const FString Key=String(O,TEXT("speaker"));const FString Turn=String(O,TEXT("turn_id"));
 if(Type==TEXT("scene")){
  auto S=O->GetObjectField(TEXT("scene"));auto D=S->GetObjectField(TEXT("definition"));Goal=String(D,TEXT("goal"));JoinPolicy=String(D,TEXT("join_policy"));S->TryGetBoolField(TEXT("joined"),bJoined);
  History.Empty();const TArray<TSharedPtr<FJsonValue>>* Lines=nullptr;
  if(S->TryGetArrayField(TEXT("history"),Lines))for(auto V:*Lines){auto L=V->AsObject();FLocalTalkerLine Line;Line.Speaker=String(L,TEXT("speaker"));Line.Name=String(L,TEXT("name"));Line.Text=String(L,TEXT("text"));History.Add(Line);}
 }
 else if(Type==TEXT("participation")){O->TryGetBoolField(TEXT("joined"),bJoined);JoinPolicy=String(O,TEXT("policy"));}
 else if(Type==TEXT("turn_start")){StopPlayback();CurrentTurn=Turn;ActiveSpeaker=Key;ActiveName=String(O,TEXT("name"));State=TEXT("Thinking");Error.Empty();}
 else if(Type==TEXT("interrupted")){StopPlayback();CurrentTurn.Empty();ActiveSpeaker.Empty();if(!bRecording)State=TEXT("Ready");}
 else if(Type==TEXT("dialogue") && Turn==CurrentTurn){StreamingText=String(O,TEXT("text"));}
 else if(Type==TEXT("audio") && Turn==CurrentTurn){
  TArray<uint8> PCM;FBase64::Decode(String(O,TEXT("pcm16_b64")),PCM);int32 Rate=24000;O->TryGetNumberField(TEXT("sample_rate"),Rate);
  for(auto C:Characters)if(C.IsValid() && C->CharacterKey==Key){C->QueueVoice(PCM,Rate);ReceivedAudioBytes+=PCM.Num();break;}
  AudioEndsAt=FMath::Max(AudioEndsAt,FPlatformTime::Seconds())+double(PCM.Num())/(2*Rate);State=TEXT("Speaking");
 }
 else if(Type==TEXT("reply") && Turn==CurrentTurn){FLocalTalkerLine Line;Line.Speaker=Key;Line.Name=String(O,TEXT("name"));Line.Text=String(O,TEXT("text"));History.Add(Line);StreamingText.Empty();LastStructuredReply=Encode(O->GetObjectField(TEXT("reply")).ToSharedRef());}
 else if(Type==TEXT("player_message")){FLocalTalkerLine Line;Line.Speaker=TEXT("player");Line.Name=String(O,TEXT("name"));Line.Text=String(O,TEXT("text"));History.Add(Line);}
 else if(Type==TEXT("turn_end") && Turn==CurrentTurn){bAwaitingPlayback=true;}
 else if(Type==TEXT("scene_state")){if(!bRecording && AudioEndsAt<FPlatformTime::Seconds())State=String(O,TEXT("state"))==TEXT("idle")?TEXT("Ready"):String(O,TEXT("state"));if(State==TEXT("Ready"))ActiveSpeaker.Empty();}
 else if(Type==TEXT("error") || Type==TEXT("warning")){Error=String(O,TEXT("error"));State=TEXT("Error");}
 OnSceneEvent.Broadcast(Json);
 UE_LOG(LogTemp,Verbose,TEXT("LocalTalker event: %s %s"),*Type,*Key);
}
