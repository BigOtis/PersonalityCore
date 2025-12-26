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
