#pragma once

#include <windows.h>
#include <atomic>
#include <filesystem>
#include <memory>

namespace refarter {
enum class CompressionStage : unsigned {
    Prepare = 1, LockSource, OpenInput, ReadStreams, Plan, Decoder, Encoder, Audio,
    Header, ReadPacket, Decode, Download, Scale, Encode, Mux, FlushDecoder,
    FlushEncoder, Trailer, CheckSize, Publish, Count
};

// Only structured events and numbers cross this boundary: no titles, paths,
// error-message strings, key presses or raw media can enter the log.
enum class DiagnosticEvent {
    SessionStarted, SessionEnded, AutoStartRequested, StartRequested,
    CaptureWindowAttempt, CaptureWindowReady, CaptureStarted, CaptureFailed,
    ReplayStopped, SaveRequested, SaveFinished, SaveFailed, RecorderClosed, EventsDropped,
    StartupStage, StartFailed, GameTarget, GameExited,
    CompressionStarted, CompressionAttempt, CompressionInputBytes, CompressionSource,
    CompressionDuration, CompressionSourceFormat, CompressionColour, CompressionPlan,
    CompressionRate, CompressionDecoder, CompressionDecodeDevice, CompressionAdapter,
    CompressionDriver, CompressionFrameFormat, CompressionEncoder, CompressionFrames,
    CompressionPackets, CompressionOutputBytes, CompressionFinished, CompressionSkipped,
    CompressionCancelled, CompressionError, CompressionStage, CompressionLibraries,
    CompressionTimeBase, CompressionFrameRate, CompressionSystemError,
    CompressionLumaSample, CompressionDecodedLuma, CompressionScaledLuma, Count
};

class DiagnosticLog {
public:
    explicit DiagnosticLog(std::size_t fileBytes = 2 * 1024 * 1024);
    ~DiagnosticLog();
    DiagnosticLog(const DiagnosticLog&) = delete;
    DiagnosticLog& operator=(const DiagnosticLog&) = delete;
    // Called on the UI thread. A disabled logger creates no thread or files.
    bool configure(bool enabled, const std::filesystem::path& folder, const char* version) noexcept;
    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
    void write(DiagnosticEvent event, HRESULT result = S_OK, unsigned first = 0, unsigned second = 0) noexcept;
    // Bounded shutdown; a slow writer retains only its shared state, never App.
    bool finish(DWORD timeoutMs = 250) noexcept;
private:
    struct State;
    std::shared_ptr<State> state_;
    std::atomic<bool> enabled_{false};
};

DiagnosticLog& diagnosticLog();
} // namespace refarter
