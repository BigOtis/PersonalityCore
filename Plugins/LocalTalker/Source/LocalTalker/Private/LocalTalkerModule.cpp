#include "Modules/ModuleManager.h"
#include "LocalTalkerLlamaCache.h"
#include "LocalTalkerLog.h"
#include "LocalTalkerSettings.h"
#include "LocalCharacterComponent.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"

namespace
{
	struct FLocalTalkerLLMIntegrationRunner : public TSharedFromThis<FLocalTalkerLLMIntegrationRunner>
	{
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<AActor> MiloActor;
		TWeakObjectPtr<AActor> OtisActor;
		TWeakObjectPtr<ULocalCharacterComponent> Milo;
		TWeakObjectPtr<ULocalCharacterComponent> Otis;

		int32 MiloSentences = 0;
		int32 OtisSentences = 0;
		double StartSeconds = 0.0;
		double TimeoutSeconds = 60.0;
		int32 Step = 0;
		FString MiloPrompt;
		FString OtisPrompt;
		FString LogTag = TEXT("ITestLLM");
		bool bRequireAudioLifecycle = false;
		bool bMiloPrevPlaying = false;
		bool bOtisPrevPlaying = false;
		bool bMiloAudioStarted = false;
		bool bMiloAudioCompleted = false;
		bool bOtisAudioStarted = false;
		bool bOtisAudioCompleted = false;

		bool bSavedSettings = false;
		bool bSavedRequireAllTalk = false;
		bool bSavedRequireAuto = false;
		ELocalTalkTtsBackend SavedTtsBackend = ELocalTalkTtsBackend::KokoroWorker;

		FTSTicker::FDelegateHandle TickHandle;

		void ObserveAudio(const TCHAR* Name, ULocalCharacterComponent* Comp, bool& bPrevPlaying, bool& bStarted, bool& bCompleted)
		{
			if (!Comp)
			{
				return;
			}

			const bool bPlaying = Comp->IsAudioPlaying();
			if (!bPrevPlaying && bPlaying)
			{
				bStarted = true;
				UE_LOG(LogLocalTalker, Log, TEXT("[%s] %s audio started."), *LogTag, Name);
			}
			if (bPrevPlaying && !bPlaying && bStarted)
			{
				bCompleted = true;
				UE_LOG(LogLocalTalker, Log, TEXT("[%s] %s audio completed."), *LogTag, Name);
			}
			bPrevPlaying = bPlaying;
		}

		void ApplyTestSettings()
		{
			if (ULocalTalkerSettings* S = GetMutableDefault<ULocalTalkerSettings>())
			{
				bSavedSettings = true;
				bSavedRequireAllTalk = S->bRequirePlayerListenerForAllTalk;
				bSavedRequireAuto = S->bRequirePlayerListenerForAuto;
				SavedTtsBackend = S->TtsBackend;

				S->bRequirePlayerListenerForAllTalk = false;
				S->bRequirePlayerListenerForAuto = false;
				S->TtsBackend = ELocalTalkTtsBackend::KokoroWorker;
			}
		}

		void RestoreTestSettings()
		{
			if (!bSavedSettings)
			{
				return;
			}

			if (ULocalTalkerSettings* S = GetMutableDefault<ULocalTalkerSettings>())
			{
				S->bRequirePlayerListenerForAllTalk = bSavedRequireAllTalk;
				S->bRequirePlayerListenerForAuto = bSavedRequireAuto;
				S->TtsBackend = SavedTtsBackend;
			}
			bSavedSettings = false;
		}

		bool HasMiloPassed() const
		{
			if (MiloSentences <= 0)
			{
				return false;
			}
			if (!bRequireAudioLifecycle)
			{
				return true;
			}
			return bMiloAudioStarted && bMiloAudioCompleted;
		}

		bool HasOtisPassed() const
		{
			if (OtisSentences <= 0)
			{
				return false;
			}
			if (!bRequireAudioLifecycle)
			{
				return true;
			}
			return bOtisAudioStarted && bOtisAudioCompleted;
		}

		void Start(
			UWorld* InWorld,
			const FString& InMiloPrompt,
			const FString& InOtisPrompt,
			double InTimeoutSeconds,
			bool bInRequireAudioLifecycle,
			const FString& InLogTag)
		{
			World = InWorld;
			MiloPrompt = InMiloPrompt;
			OtisPrompt = InOtisPrompt;
			TimeoutSeconds = InTimeoutSeconds;
			bRequireAudioLifecycle = bInRequireAudioLifecycle;
			LogTag = InLogTag;
			StartSeconds = FPlatformTime::Seconds();
			Step = 0;
			MiloSentences = 0;
			OtisSentences = 0;
			bMiloPrevPlaying = false;
			bOtisPrevPlaying = false;
			bMiloAudioStarted = false;
			bMiloAudioCompleted = false;
			bOtisAudioStarted = false;
			bOtisAudioCompleted = false;

			if (!InWorld)
			{
				UE_LOG(LogLocalTalker, Error, TEXT("[%s] No UWorld provided."), *LogTag);
				return;
			}

			ApplyTestSettings();

			AActor* A = InWorld->SpawnActor<AActor>();
			AActor* B = InWorld->SpawnActor<AActor>();
			MiloActor = A;
			OtisActor = B;

			if (!A || !B)
			{
				UE_LOG(LogLocalTalker, Error, TEXT("[%s] Failed to spawn test actors."), *LogTag);
				Cleanup();
				return;
			}

			ULocalCharacterComponent* MiloComp = NewObject<ULocalCharacterComponent>(A);
			ULocalCharacterComponent* OtisComp = NewObject<ULocalCharacterComponent>(B);

			MiloComp->RegisterComponent();
			OtisComp->RegisterComponent();

			// Configure from project settings (and allow LocalCharacterComponent::ResolvePaths to fill plugin defaults).
			MiloComp->bUseProjectSettingsPaths = true;
			MiloComp->bUseProjectSettingsConfig = true;
			OtisComp->bUseProjectSettingsPaths = true;
			OtisComp->bUseProjectSettingsConfig = true;

			MiloComp->SpeakerName = TEXT("Milo");
			OtisComp->SpeakerName = TEXT("Otis");

			// Optional: place them near each other.
			A->SetActorLocation(FVector(0, 0, 0));
			B->SetActorLocation(FVector(100, 0, 0));

			Milo = MiloComp;
			Otis = OtisComp;

			MiloComp->OnSubtitleNative.AddLambda([this](const FString& /*Speaker*/, const FString& Text)
			{
				MiloSentences++;
				UE_LOG(LogLocalTalker, Log, TEXT("[%s] Milo -> '%s'"), *LogTag, *Text);
			});
			OtisComp->OnSubtitleNative.AddLambda([this](const FString& /*Speaker*/, const FString& Text)
			{
				OtisSentences++;
				UE_LOG(LogLocalTalker, Log, TEXT("[%s] Otis -> '%s'"), *LogTag, *Text);
			});

			UE_LOG(LogLocalTalker, Log, TEXT("[%s] Started. RequireAudioLifecycle=%d. Waiting to trigger Milo prompt..."),
				*LogTag,
				bRequireAudioLifecycle ? 1 : 0);
			TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(AsShared(), &FLocalTalkerLLMIntegrationRunner::Tick), 0.0f);
		}

		bool Tick(float /*DeltaSeconds*/)
		{
			UWorld* W = World.Get();
			if (!W)
			{
				UE_LOG(LogLocalTalker, Warning, TEXT("[%s] World invalid; stopping."), *LogTag);
				Cleanup();
				return false;
			}

			const double Now = FPlatformTime::Seconds();
			if ((Now - StartSeconds) > TimeoutSeconds)
			{
				UE_LOG(LogLocalTalker, Error,
					TEXT("[%s] TIMEOUT after %.1fs (MiloSentences=%d OtisSentences=%d MiloAudioStart=%d MiloAudioDone=%d OtisAudioStart=%d OtisAudioDone=%d)."),
					*LogTag,
					TimeoutSeconds,
					MiloSentences,
					OtisSentences,
					bMiloAudioStarted ? 1 : 0,
					bMiloAudioCompleted ? 1 : 0,
					bOtisAudioStarted ? 1 : 0,
					bOtisAudioCompleted ? 1 : 0);
				Cleanup();
				return false;
			}

			ULocalCharacterComponent* MiloComp = Milo.Get();
			ULocalCharacterComponent* OtisComp = Otis.Get();
			if (!MiloComp || !OtisComp)
			{
				UE_LOG(LogLocalTalker, Error, TEXT("[%s] Components invalid; stopping."), *LogTag);
				Cleanup();
				return false;
			}

			ObserveAudio(TEXT("Milo"), MiloComp, bMiloPrevPlaying, bMiloAudioStarted, bMiloAudioCompleted);
			ObserveAudio(TEXT("Otis"), OtisComp, bOtisPrevPlaying, bOtisAudioStarted, bOtisAudioCompleted);

			// Steps:
			// 0) Start Milo LLM+TTS
			// 1) Wait Milo produces at least one sentence (+ audio lifecycle if required)
			// 2) Start Otis LLM+TTS
			// 3) Wait Otis produces at least one sentence (+ audio lifecycle if required)
			// 4) Done
			switch (Step)
			{
			case 0:
				UE_LOG(LogLocalTalker, Log, TEXT("[%s] Trigger Milo prompt: %s"), *LogTag, *MiloPrompt);
				MiloComp->SendPromptAndSpeakStreamingInProc(MiloPrompt);
				Step = 1;
				break;
			case 1:
				if (HasMiloPassed())
				{
					UE_LOG(LogLocalTalker, Log,
						TEXT("[%s] Milo passed (sentences=%d audioStart=%d audioDone=%d). Triggering Otis..."),
						*LogTag,
						MiloSentences,
						bMiloAudioStarted ? 1 : 0,
						bMiloAudioCompleted ? 1 : 0);
					Step = 2;
				}
				break;
			case 2:
				UE_LOG(LogLocalTalker, Log, TEXT("[%s] Trigger Otis prompt: %s"), *LogTag, *OtisPrompt);
				OtisComp->SendPromptAndSpeakStreamingInProc(OtisPrompt);
				Step = 3;
				break;
			case 3:
				if (HasOtisPassed())
				{
					UE_LOG(LogLocalTalker, Log,
						TEXT("[%s] SUCCESS (MiloSentences=%d OtisSentences=%d MiloAudioStart=%d MiloAudioDone=%d OtisAudioStart=%d OtisAudioDone=%d)."),
						*LogTag,
						MiloSentences,
						OtisSentences,
						bMiloAudioStarted ? 1 : 0,
						bMiloAudioCompleted ? 1 : 0,
						bOtisAudioStarted ? 1 : 0,
						bOtisAudioCompleted ? 1 : 0);
					Cleanup();
					return false;
				}
				break;
			default:
				break;
			}

			return true;
		}

		void Cleanup()
		{
			if (TickHandle.IsValid())
			{
				FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
				TickHandle.Reset();
			}

			RestoreTestSettings();

			if (AActor* A = MiloActor.Get())
			{
				A->Destroy();
			}
			if (AActor* B = OtisActor.Get())
			{
				B->Destroy();
			}

			Milo.Reset();
			Otis.Reset();
			MiloActor.Reset();
			OtisActor.Reset();
			World.Reset();
		}
	};

	static TSharedPtr<FLocalTalkerLLMIntegrationRunner> GLLMRunner;

	static void LocalTalker_RunITest(const TArray<FString>& Args, UWorld* World, bool bRequireAudioByDefault, const TCHAR* Tag)
	{
		if (!World)
		{
			UE_LOG(LogLocalTalker, Error, TEXT("[%s] No world available. Run this from PIE/Standalone in-game console."), Tag);
			return;
		}

		if (GLLMRunner.IsValid())
		{
			UE_LOG(LogLocalTalker, Warning, TEXT("[%s] Already running. Wait for completion or restart PIE."), Tag);
			return;
		}

		FString MiloPrompt = TEXT("Say hello to Otis in exactly one short sentence.");
		FString OtisPrompt = TEXT("Reply to Milo in exactly one short sentence.");
		double Timeout = 60.0;
		bool bRequireAudio = bRequireAudioByDefault;

		for (const FString& A : Args)
		{
			if (A.StartsWith(TEXT("milo=")))
			{
				MiloPrompt = A.Mid(5);
			}
			else if (A.StartsWith(TEXT("otis=")))
			{
				OtisPrompt = A.Mid(5);
			}
			else if (A.StartsWith(TEXT("timeout=")))
			{
				Timeout = FCString::Atod(*A.Mid(8));
			}
			else if (A.StartsWith(TEXT("audio=")))
			{
				bRequireAudio = FCString::Atoi(*A.Mid(6)) != 0;
			}
		}

		GLLMRunner = MakeShared<FLocalTalkerLLMIntegrationRunner>();
		GLLMRunner->Start(World, MiloPrompt, OtisPrompt, Timeout, bRequireAudio, FString(Tag));
	}

	static void LocalTalker_RunITestLLM(const TArray<FString>& Args, UWorld* World)
	{
		LocalTalker_RunITest(Args, World, /*bRequireAudioByDefault*/ false, TEXT("ITestLLM"));
	}

	static void LocalTalker_RunITestE2E(const TArray<FString>& Args, UWorld* World)
	{
		LocalTalker_RunITest(Args, World, /*bRequireAudioByDefault*/ true, TEXT("ITestE2E"));
	}

	static FAutoConsoleCommandWithWorldAndArgs CCmdLocalTalkerITestLLM(
		TEXT("LocalTalker.ITestLLM"),
		TEXT("Integration test: spawns two LocalTalk actors and runs LLM+TTS (text-focused).\n")
		TEXT("Usage:\n")
		TEXT("  LocalTalker.ITestLLM\n")
		TEXT("  LocalTalker.ITestLLM milo=<prompt> otis=<prompt> timeout=<seconds> audio=<0|1>\n")
		TEXT("Notes:\n")
		TEXT("- Default audio=0 for backwards compatibility.\n")
		TEXT("- Requires Project Settings -> LocalTalker DefaultPaths LlamaLibPath + LlamaModelPath to be set.\n")
		TEXT("- Kokoro TTS runs CPU-only via ONNX; no extra paths needed."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LocalTalker_RunITestLLM)
	);

	static FAutoConsoleCommandWithWorldAndArgs CCmdLocalTalkerITestE2E(
		TEXT("LocalTalker.ITestE2E"),
		TEXT("End-to-end integration test: validates LLM text + TTS audio lifecycle.\n")
		TEXT("Usage:\n")
		TEXT("  LocalTalker.ITestE2E\n")
		TEXT("  LocalTalker.ITestE2E milo=<prompt> otis=<prompt> timeout=<seconds> audio=<0|1>\n")
		TEXT("Notes:\n")
		TEXT("- Default audio=1. Success requires sentence + audio start + audio completion for both speakers.\n")
		TEXT("- Requires Project Settings -> LocalTalker DefaultPaths LlamaLibPath + LlamaModelPath to be set.\n")
		TEXT("- Kokoro TTS runs CPU-only via ONNX; no extra paths needed."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LocalTalker_RunITestE2E)
	);
}

class FLocalTalkerModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
	{
		UE_LOG(LogLocalTalker, Log, TEXT("[LocalTalker] Module loaded. Consoles: LocalTalker.ITestLLM, LocalTalker.ITestE2E"));
	}
    virtual void ShutdownModule() override
    {
        // Ensure we release llama.cpp resources on shutdown (editor/game exit).
        FLocalTalkerLlamaCache::Get().Shutdown();
        ULocalCharacterComponent::ShutdownSharedTtsWorkerGlobal();

		// Ensure runner is cleaned up if hot-reloading/module shutdown occurs.
		if (GLLMRunner.IsValid())
		{
			GLLMRunner->Cleanup();
			GLLMRunner.Reset();
		}
    }
};

IMPLEMENT_MODULE(FLocalTalkerModule, LocalTalker)
