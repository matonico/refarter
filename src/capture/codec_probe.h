#pragma once

#include "video_settings.h"

#include <windows.h>
#include <string>
#include <vector>

namespace refarter::capture {

struct CodecSupport {
    Codec codec = Codec::H264;
    bool records = false;  // The GPU encoded a short clip and Windows wrote and reopened it.
    bool plays = false;    // Media Foundation decoded a test frame; not a test of third-party players.
    std::wstring encoder;  // Hardware encoder name, when one opened.
    std::wstring problem;  // Why recording is unavailable.
};

// Checks each codec end to end on the GPU driving `monitor`: opens the hardware
// encoder exactly as recording would, encodes a few blank frames, writes them
// as an MP4 and reads it back. Takes up to a second or so; call it from a
// worker, only while replay is off, and cache the result.
std::vector<CodecSupport> probeCodecs(HMONITOR monitor);

struct Gpu;
} // namespace refarter::capture

struct ID3D11Device;

namespace refarter::capture {

// Codecs that refused 10-bit on one GPU (by adapter LUID): in a check, at a start, before a recording's first packet
// or after a resize. Later starts and checks on that GPU use 8-bit; other GPUs still try 10-bit. Until Refarter
// closes; any thread.
void refuseTenBit(Codec codec, LUID gpu);
bool tenBitRefused(Codec codec, LUID gpu);

// A 10-bit encoder failed before its first packet. The first time, the next 10-bit start checks again; the second
// time on this GPU and codec, 10-bit is refused there. Returns whether it is now refused.
bool tenBitEncoderFailed(Codec codec, LUID gpu);

// A lost device, exhausted memory, a cancel, a busy encoder (too slow, or failing to start streaming): never a
// 10-bit refusal, whatever 8-bit then does.
bool notTenBitsFault(HRESULT error, ID3D11Device* device);

struct TenBitWriteCheck {
    bool writes = false;  // 10-bit recorded, wrote and read back on this GPU; remembered for it and the codec.
    bool refusal = false; // It failed in a way that blames 10-bit: the caller refuses it once 8-bit opens.
    HRESULT error = S_OK; // Why it failed, for the log.
};

// Before a 10-bit start: encodes a short 10-bit clip on `gpu`, writes it as MP4 and reads it back, as the codec
// check does, so a writer that refuses 10-bit is found before a save fails. About a second, the first time only;
// stops early when `cancel` is set. Media Foundation must be started.
TenBitWriteCheck checkTenBitWrites(Codec codec, const Gpu& gpu, HANDLE cancel);

} // namespace refarter::capture
