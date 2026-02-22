#include "LocalTalkerProcess.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

bool FLocalTalkerProcess::SpawnWithPipes(const FString& ExePath, const FString& Args, const FString& WorkingDir,
                                        FProcHandle& OutHandle, FLocalProcPipes& OutPipes, FString& OutError)
{
    const bool bLooksLikePath = ExePath.Contains(TEXT("/")) || ExePath.Contains(TEXT("\\")) || ExePath.Contains(TEXT(":"));
    if (bLooksLikePath && !FPaths::FileExists(ExePath))
    {
        OutError = FString::Printf(TEXT("Executable not found: %s"), *ExePath);
        return false;
    }

    FPlatformProcess::CreatePipe(OutPipes.ReadPipe, OutPipes.WritePipe, true);
    FPlatformProcess::CreatePipe(OutPipes.ReadErrPipe, OutPipes.WriteErrPipe, true);
    FPlatformProcess::CreatePipe(OutPipes.ReadInPipe, OutPipes.WriteInPipe, true);

    uint32 ProcessId = 0;

    OutHandle = FPlatformProcess::CreateProc(
        *ExePath,
        *Args,
        true,
        false,
        false,
        &ProcessId,
        0,
        WorkingDir.IsEmpty() ? nullptr : *WorkingDir,
        OutPipes.WritePipe,
        OutPipes.ReadInPipe,
        OutPipes.WriteErrPipe
    );

    if (!OutHandle.IsValid())
    {
        OutError = FString::Printf(TEXT("Failed to spawn process: %s %s"), *ExePath, *Args);
        ClosePipes(OutPipes);
        return false;
    }

    return true;
}

void FLocalTalkerProcess::ClosePipes(FLocalProcPipes& Pipes)
{
    if (Pipes.ReadPipe || Pipes.WritePipe)
    {
        FPlatformProcess::ClosePipe(Pipes.ReadPipe, Pipes.WritePipe);
        Pipes.ReadPipe = nullptr;
        Pipes.WritePipe = nullptr;
    }

    if (Pipes.ReadErrPipe || Pipes.WriteErrPipe)
    {
        FPlatformProcess::ClosePipe(Pipes.ReadErrPipe, Pipes.WriteErrPipe);
        Pipes.ReadErrPipe = nullptr;
        Pipes.WriteErrPipe = nullptr;
    }

    if (Pipes.ReadInPipe || Pipes.WriteInPipe)
    {
        FPlatformProcess::ClosePipe(Pipes.ReadInPipe, Pipes.WriteInPipe);
        Pipes.ReadInPipe = nullptr;
        Pipes.WriteInPipe = nullptr;
    }
}

bool FLocalTalkerProcess::WriteStdin(FLocalProcPipes& Pipes, const FString& TextUtf8)
{
    if (!Pipes.WriteInPipe) return false;
    return FPlatformProcess::WritePipe(Pipes.WriteInPipe, TextUtf8);
}

FString FLocalTalkerProcess::ReadAvailable(void* ReadPipe)
{
    if (!ReadPipe) return FString();
    return FPlatformProcess::ReadPipe(ReadPipe);
}

void FLocalTalkerProcess::PumpOutputUntilExit(FProcHandle& Handle, FLocalProcPipes& Pipes,
                                             const FOnStdout& OnOut, const FOnStderr& OnErr, double PollSeconds)
{
    while (Handle.IsValid() && FPlatformProcess::IsProcRunning(Handle))
    {
        const FString Out = ReadAvailable(Pipes.ReadPipe);
        if (!Out.IsEmpty() && OnOut) OnOut(Out);

        const FString Err = ReadAvailable(Pipes.ReadErrPipe);
        if (!Err.IsEmpty() && OnErr) OnErr(Err);

        FPlatformProcess::Sleep(static_cast<float>(PollSeconds));
    }

    const FString OutFinal = ReadAvailable(Pipes.ReadPipe);
    if (!OutFinal.IsEmpty() && OnOut) OnOut(OutFinal);

    const FString ErrFinal = ReadAvailable(Pipes.ReadErrPipe);
    if (!ErrFinal.IsEmpty() && OnErr) OnErr(ErrFinal);
}
