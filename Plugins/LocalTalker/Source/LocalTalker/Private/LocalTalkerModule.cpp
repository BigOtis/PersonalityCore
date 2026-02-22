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

		FTSTicker::FDelegateHandle TickHandle;

		void Start(UWorld* InWorld, const FString& InMiloPrompt, const FString& InOtisPrompt, double InTimeoutSeconds)
		{
			World = InWorld;
			MiloPrompt = InMiloPrompt;
			OtisPrompt = InOtisPrompt;
			TimeoutSeconds = InTimeoutSeconds;
			StartSeconds = FPlatformTime::Seconds();
			Step = 0;
			MiloSentences = 0;
			OtisSentences = 0;

			if (!InWorld)
			{
				UE_LOG(LogLocalTalker, Error, TEXT("[ITestLLM] No UWorld provided."));
				return;
			}

			AActor* A = InWorld->SpawnActor<AActor>();
			AActor* B = InWorld->SpawnActor<AActor>();
			MiloActor = A;
			OtisActor = B;

			if (!A || !B)
			{
				UE_LOG(LogLocalTalker, Error, TEXT("[ITestLLM] Failed to spawn test actors."));
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
				UE_LOG(LogLocalTalker, Log, TEXT("[ITestLLM] Milo -> '%s'"), *Text);
			});
			OtisComp->OnSubtitleNative.AddLambda([this](const FString& /*Speaker*/, const FString& Text)
			{
				OtisSentences++;
				UE_LOG(LogLocalTalker, Log, TEXT("[ITestLLM] Otis -> '%s'"), *Text);
			});

			UE_LOG(LogLocalTalker, Log, TEXT("[ITestLLM] Started. Waiting to trigger Milo prompt..."));
			TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(AsShared(), &FLocalTalkerLLMIntegrationRunner::Tick), 0.0f);
		}

		bool Tick(float /*DeltaSeconds*/)
		{
			UWorld* W = World.Get();
			if (!W)
			{
				UE_LOG(LogLocalTalker, Warning, TEXT("[ITestLLM] World invalid; stopping."));
				Cleanup();
				return false;
			}

			const double Now = FPlatformTime::Seconds();
			if ((Now - StartSeconds) > TimeoutSeconds)
			{
				UE_LOG(LogLocalTalker, Error, TEXT("[ITestLLM] TIMEOUT after %.1fs (MiloSentences=%d OtisSentences=%d)."), TimeoutSeconds, MiloSentences, OtisSentences);
				Cleanup();
				return false;
			}

			ULocalCharacterComponent* MiloComp = Milo.Get();
			ULocalCharacterComponent* OtisComp = Otis.Get();
			if (!MiloComp || !OtisComp)
			{
				UE_LOG(LogLocalTalker, Error, TEXT("[ITestLLM] Components invalid; stopping."));
				Cleanup();
				return false;
			}

			// Steps:
			// 0) Start Milo LLM+TTS
			// 1) Wait Milo produces at least one sentence
			// 2) Start Otis LLM+TTS
			// 3) Wait Otis produces at least one sentence
			// 4) Done
			switch (Step)
			{
			case 0:
				UE_LOG(LogLocalTalker, Log, TEXT("[ITestLLM] Trigger Milo prompt: %s"), *MiloPrompt);
				MiloComp->SendPromptAndSpeakStreamingInProc(MiloPrompt);
				Step = 1;
				break;
			case 1:
				if (MiloSentences > 0)
				{
					UE_LOG(LogLocalTalker, Log, TEXT("[ITestLLM] Milo spoke (%d sentence(s)). Triggering Otis..."), MiloSentences);
					Step = 2;
				}
				break;
			case 2:
				UE_LOG(LogLocalTalker, Log, TEXT("[ITestLLM] Trigger Otis prompt: %s"), *OtisPrompt);
				OtisComp->SendPromptAndSpeakStreamingInProc(OtisPrompt);
				Step = 3;
				break;
			case 3:
				if (OtisSentences > 0)
				{
					UE_LOG(LogLocalTalker, Log, TEXT("[ITestLLM] SUCCESS (MiloSentences=%d OtisSentences=%d)."), MiloSentences, OtisSentences);
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

	static void LocalTalker_RunITestLLM(const TArray<FString>& Args, UWorld* World)
	{
		if (!World)
		{
			UE_LOG(LogLocalTalker, Error, TEXT("[ITestLLM] No world available. Run this from PIE/Standalone in-game console."));
			return;
		}

		if (GLLMRunner.IsValid())
		{
			UE_LOG(LogLocalTalker, Warning, TEXT("[ITestLLM] Already running. Wait for completion or restart PIE."));
			return;
		}

		FString MiloPrompt = TEXT("Say hello to Otis in exactly one short sentence.");
		FString OtisPrompt = TEXT("Reply to Milo in exactly one short sentence.");
		double Timeout = 60.0;

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
		}

		GLLMRunner = MakeShared<FLocalTalkerLLMIntegrationRunner>();
		GLLMRunner->Start(World, MiloPrompt, OtisPrompt, Timeout);
	}

	static FAutoConsoleCommandWithWorldAndArgs CCmdLocalTalkerITestLLM(
		TEXT("LocalTalker.ITestLLM"),
		TEXT("Integration test: spawns two LocalTalk actors and runs LLM+TTS.\n")
		TEXT("Usage:\n")
		TEXT("  LocalTalker.ITestLLM\n")
		TEXT("  LocalTalker.ITestLLM milo=<prompt> otis=<prompt> timeout=<seconds>\n")
		TEXT("Notes:\n")
		TEXT("- Requires Project Settings -> LocalTalker DefaultPaths LlamaLibPath + LlamaModelPath to be set.\n")
		TEXT("- Qwen worker/model paths can be set in Project Settings -> LocalTalker -> DefaultPaths."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&LocalTalker_RunITestLLM)
	);
}

class FLocalTalkerModule : public IModuleInterface
{
public:
    virtual void StartupModule() override
	{
		UE_LOG(LogLocalTalker, Log, TEXT("[LocalTalker] Module loaded. Console: LocalTalker.ITestLLM"));
	}
    virtual void ShutdownModule() override
    {
        // Ensure we release llama.cpp resources on shutdown (editor/game exit).
        FLocalTalkerLlamaCache::Get().Shutdown();

		// Ensure runner is cleaned up if hot-reloading/module shutdown occurs.
		if (GLLMRunner.IsValid())
		{
			GLLMRunner->Cleanup();
			GLLMRunner.Reset();
		}
    }
};

IMPLEMENT_MODULE(FLocalTalkerModule, LocalTalker)
