#pragma once
#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"

struct FLocalProcPipes
{
    void* ReadPipe = nullptr;
    void* WritePipe = nullptr;
    void* ReadErrPipe = nullptr;
    void* WriteErrPipe = nullptr;
    void* ReadInPipe = nullptr;
    void* WriteInPipe = nullptr;
};

class FLocalTalkerProcess
{
public:
    using FOnStdout = TFunction<void(const FString&)>;
    using FOnStderr = TFunction<void(const FString&)>;

    static bool SpawnWithPipes(const FString& ExePath, const FString& Args, const FString& WorkingDir,
                              FProcHandle& OutHandle, FLocalProcPipes& OutPipes, FString& OutError);

    static void ClosePipes(FLocalProcPipes& Pipes);
    static bool WriteStdin(FLocalProcPipes& Pipes, const FString& TextUtf8);
    static void PumpOutputUntilExit(FProcHandle& Handle, FLocalProcPipes& Pipes,
                                   const FOnStdout& OnOut, const FOnStderr& OnErr, double PollSeconds = 0.01);
    static FString ReadAvailable(void* ReadPipe);
};
