#include "LocalTalkerWav.h"
#include "Misc/FileHelper.h"

static uint32 ReadU32LE(const uint8* P) { return (uint32)P[0] | ((uint32)P[1] << 8) | ((uint32)P[2] << 16) | ((uint32)P[3] << 24); }
static uint16 ReadU16LE(const uint8* P) { return (uint16)P[0] | ((uint16)P[1] << 8); }

bool FLocalTalkerWav::LoadWavPcm16(const FString& Path, FLocalWavPcm16& Out, FString& OutError)
{
    TArray<uint8> Data;
    if (!FFileHelper::LoadFileToArray(Data, *Path))
    {
        OutError = FString::Printf(TEXT("Failed to read wav: %s"), *Path);
        return false;
    }
    if (Data.Num() < 44) { OutError = TEXT("WAV too small"); return false; }

    const uint8* P = Data.GetData();
    if (FMemory::Memcmp(P, "RIFF", 4) != 0 || FMemory::Memcmp(P + 8, "WAVE", 4) != 0)
    {
        OutError = TEXT("Not a RIFF/WAVE file");
        return false;
    }

    int32 Offset = 12;
    uint16 AudioFormat = 0, NumChannels = 0, BitsPerSample = 0;
    uint32 SampleRate = 0;
    const uint8* DataChunk = nullptr;
    uint32 DataChunkSize = 0;

    while (Offset + 8 <= Data.Num())
    {
        const char* ChunkId = (const char*)(P + Offset);
        uint32 ChunkSize = ReadU32LE(P + Offset + 4);
        Offset += 8;
        if (Offset + (int32)ChunkSize > Data.Num()) break;

        if (FMemory::Memcmp(ChunkId, "fmt ", 4) == 0)
        {
            if (ChunkSize < 16) { OutError = TEXT("Invalid fmt chunk"); return false; }
            AudioFormat = ReadU16LE(P + Offset + 0);
            NumChannels = ReadU16LE(P + Offset + 2);
            SampleRate = ReadU32LE(P + Offset + 4);
            BitsPerSample = ReadU16LE(P + Offset + 14);
        }
        else if (FMemory::Memcmp(ChunkId, "data", 4) == 0)
        {
            DataChunk = P + Offset;
            DataChunkSize = ChunkSize;
        }

        Offset += (int32)ChunkSize;
        if (ChunkSize % 2 == 1) Offset += 1;
    }

    if (!DataChunk || DataChunkSize == 0) { OutError = TEXT("Missing data chunk"); return false; }
    if (AudioFormat != 1) { OutError = TEXT("Only PCM supported"); return false; }
    if (BitsPerSample != 16) { OutError = TEXT("Only 16-bit PCM supported"); return false; }

    const int32 NumSamples = (int32)(DataChunkSize / 2);
    Out.Samples.SetNumUninitialized(NumSamples);
    FMemory::Memcpy(Out.Samples.GetData(), DataChunk, NumSamples * 2);
    Out.SampleRate = (int32)SampleRate;
    Out.NumChannels = (int32)NumChannels;
    return true;
}

bool FLocalTalkerWav::SaveWavPcm16(const FString& Path, const FLocalWavPcm16& In, FString& OutError)
{
    OutError.Reset();
    if (In.SampleRate <= 0)
    {
        OutError = TEXT("Invalid sample rate.");
        return false;
    }
    if (In.NumChannels <= 0)
    {
        OutError = TEXT("Invalid channel count.");
        return false;
    }
    if (In.Samples.Num() <= 0)
    {
        OutError = TEXT("No PCM samples to write.");
        return false;
    }

    const uint16 BitsPerSample = 16;
    const uint32 DataSizeBytes = static_cast<uint32>(In.Samples.Num() * sizeof(int16));
    const uint32 FmtChunkSize = 16;
    const uint32 RiffChunkSize = 4 + (8 + FmtChunkSize) + (8 + DataSizeBytes);
    const uint32 ByteRate = static_cast<uint32>(In.SampleRate * In.NumChannels * (BitsPerSample / 8));
    const uint16 BlockAlign = static_cast<uint16>(In.NumChannels * (BitsPerSample / 8));

    TArray<uint8> Bytes;
    Bytes.Reserve(static_cast<int32>(44 + DataSizeBytes));

    auto WriteU16 = [&Bytes](uint16 Value)
    {
        Bytes.Add(static_cast<uint8>(Value & 0xFF));
        Bytes.Add(static_cast<uint8>((Value >> 8) & 0xFF));
    };
    auto WriteU32 = [&Bytes](uint32 Value)
    {
        Bytes.Add(static_cast<uint8>(Value & 0xFF));
        Bytes.Add(static_cast<uint8>((Value >> 8) & 0xFF));
        Bytes.Add(static_cast<uint8>((Value >> 16) & 0xFF));
        Bytes.Add(static_cast<uint8>((Value >> 24) & 0xFF));
    };
    auto WriteChars = [&Bytes](const char* C, int32 Count)
    {
        for (int32 i = 0; i < Count; ++i)
        {
            Bytes.Add(static_cast<uint8>(C[i]));
        }
    };

    WriteChars("RIFF", 4);
    WriteU32(RiffChunkSize);
    WriteChars("WAVE", 4);

    WriteChars("fmt ", 4);
    WriteU32(FmtChunkSize);
    WriteU16(1); // PCM
    WriteU16(static_cast<uint16>(In.NumChannels));
    WriteU32(static_cast<uint32>(In.SampleRate));
    WriteU32(ByteRate);
    WriteU16(BlockAlign);
    WriteU16(BitsPerSample);

    WriteChars("data", 4);
    WriteU32(DataSizeBytes);

    const uint8* SampleBytes = reinterpret_cast<const uint8*>(In.Samples.GetData());
    Bytes.Append(SampleBytes, static_cast<int32>(DataSizeBytes));

    if (!FFileHelper::SaveArrayToFile(Bytes, *Path))
    {
        OutError = FString::Printf(TEXT("Failed to write wav: %s"), *Path);
        return false;
    }
    return true;
}
