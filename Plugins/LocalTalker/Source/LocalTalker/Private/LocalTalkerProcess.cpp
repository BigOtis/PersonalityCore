#include "LocalTalkerProcess.h"
#include "LocalTalkerLog.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

bool FLocalTalkerProcess::SpawnWithPipes(const FString& ExePath, const FString& Args, const FString& WorkingDir,
                                        FProcHandle& OutHandle, FLocalProcPipes& OutPipes, FString& OutError)
{
    UE_LOG(LogLocalTalker, Log, TEXT("[Proc] SpawnWithPipes exe='%s' cwd='%s' args='%s'"),
        *ExePath, *WorkingDir, *Args);

    const bool bLooksLikePath = ExePath.Contains(TEXT("/")) || ExePath.Contains(TEXT("\\")) || ExePath.Contains(TEXT(":"));
    if (bLooksLikePath && !FPaths::FileExists(ExePath))
    {
        OutError = FString::Printf(TEXT("Executable not found: %s"), *ExePath);
        UE_LOG(LogLocalTalker, Error, TEXT("[Proc] %s"), *OutError);
        return false;
    }

    // stdout/stderr are child->parent streams, so child write handles must be inheritable (bWritePipeLocal=false).
    const bool bStdOutPipeOk = FPlatformProcess::CreatePipe(OutPipes.ReadPipe, OutPipes.WritePipe, false);
    const bool bStdErrPipeOk = FPlatformProcess::CreatePipe(OutPipes.ReadErrPipe, OutPipes.WriteErrPipe, false);
    // stdin is parent->child stream, so parent write handle stays local (bWritePipeLocal=true).
    const bool bStdInPipeOk = FPlatformProcess::CreatePipe(OutPipes.ReadInPipe, OutPipes.WriteInPipe, true);

    if (!bStdOutPipeOk || !bStdErrPipeOk || !bStdInPipeOk)
    {
        OutError = FString::Printf(TEXT("Failed to create process pipes (stdout=%d stderr=%d stdin=%d)."),
            bStdOutPipeOk ? 1 : 0,
            bStdErrPipeOk ? 1 : 0,
            bStdInPipeOk ? 1 : 0);
        UE_LOG(LogLocalTalker, Error, TEXT("[Proc] %s"), *OutError);
        ClosePipes(OutPipes);
        return false;
    }

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
        UE_LOG(LogLocalTalker, Error, TEXT("[Proc] %s"), *OutError);
        ClosePipes(OutPipes);
        return false;
    }

    UE_LOG(LogLocalTalker, Log, TEXT("[Proc] Spawned pid=%u (stdout/stderr/stdin wired)"), ProcessId);
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
