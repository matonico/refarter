#include "diagnostic_log.h"

#include <array>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace refarter {
namespace {
constexpr std::array names{
    "session started", "session ended", "auto-capture requested", "capture requested",
    "capture-window attempt", "capture-window ready", "capture started", "capture failed",
    "replay stopped", "save requested", "save finished", "save failed", "recorder closed", "events dropped",
    "startup stage", "start failed", "game target", "game exited",
    "compression started", "compression attempt", "compression input size", "compression source",
    "compression duration", "compression source format", "compression colour", "compression plan",
    "compression rate", "compression decoder", "compression decode device", "compression decode adapter",
    "compression decode driver", "compression first picture", "compression encoder", "compression frames",
    "compression packets", "compression output size", "compression finished", "compression skipped",
    "compression cancelled", "compression error", "compression stage", "compression libraries",
    "compression time base", "compression source frame rate", "compression system error",
    "compression luma sample", "compression decoded luma", "compression scaled luma"
};
static_assert(names.size() == static_cast<std::size_t>(DiagnosticEvent::Count));
struct Entry { SYSTEMTIME time; DiagnosticEvent event; HRESULT result; unsigned first, second; };
constexpr std::array compressionStages{
    "unknown", "prepare", "lock_source", "open_input", "read_streams", "plan", "decoder", "encoder", "audio",
    "header", "read_packet", "decode", "download", "scale", "encode", "mux", "flush_decoder",
    "flush_encoder", "trailer", "check_size", "publish"
};
static_assert(compressionStages.size() == static_cast<std::size_t>(CompressionStage::Count));

bool append(const std::filesystem::path& folder, const std::string& line, std::size_t limit) {
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) return false;
    const auto current = folder / L"session.log";
    const auto previous = folder / L"session.1.log";
    const auto oldest = folder / L"session.2.log";
    const auto size = std::filesystem::file_size(current, error);
    if (error && error != std::errc::no_such_file_or_directory) return false;
    if (!error && size + line.size() > limit) {
        std::filesystem::remove(oldest, error);
        if (error) return false;
        if (std::filesystem::exists(previous, error)) std::filesystem::rename(previous, oldest, error);
        if (error) return false;
        std::filesystem::rename(current, previous, error);
        if (error) return false;
    }
    std::ofstream stream(current, std::ios::binary | std::ios::app);
    stream.write(line.data(), static_cast<std::streamsize>(line.size()));
    stream.flush();
    return stream.good();
}
} // namespace

struct DiagnosticLog::State {
    explicit State(std::size_t bytes) : limit(bytes < 512 ? 512 : bytes) {}
    std::mutex mutex;
    std::condition_variable ready, finished;
    std::array<Entry, 128> queue{};
    std::size_t head = 0, count = 0, limit;
    std::atomic<unsigned> dropped{0};
    bool started = false, stopping = false, done = false;
    std::filesystem::path folder;
    std::string version;

    void run() noexcept {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        try {
            for (;;) {
                Entry entry{};
                {
                    std::unique_lock lock(mutex);
                    ready.wait(lock, [&] { return stopping || count; });
                    if (!count && stopping) break;
                    entry = queue[head];
                    head = (head + 1) % queue.size();
                    --count;
                }
                char line[384]{};
                char detail[96]{};
                char errorCode[32]{};
                if (FAILED(entry.result)) {
                    const bool ffmpeg = entry.event == DiagnosticEvent::CompressionError ||
                        entry.event == DiagnosticEvent::CompressionDecoder || entry.event == DiagnosticEvent::CompressionDecodeDevice ||
                        entry.event == DiagnosticEvent::CompressionEncoder;
                    if (ffmpeg) std::snprintf(errorCode, sizeof(errorCode), " ffmpeg_error=%ld", static_cast<long>(entry.result));
                    else std::snprintf(errorCode, sizeof(errorCode), " hr=0x%08lX", static_cast<unsigned long>(entry.result));
                }
                const char* severity = entry.event == DiagnosticEvent::CaptureFailed ||
                    entry.event == DiagnosticEvent::StartFailed || entry.event == DiagnosticEvent::SaveFailed ||
                    entry.event == DiagnosticEvent::CompressionError || entry.event == DiagnosticEvent::CompressionSystemError ? "error" :
                    FAILED(entry.result) ? "warn" : "info";
                switch (entry.event) {
                case DiagnosticEvent::SessionStarted:
                    std::snprintf(detail, sizeof(detail), "version=%s pid=%lu", version.c_str(), GetCurrentProcessId()); break;
                case DiagnosticEvent::StartRequested:
                    std::snprintf(detail, sizeof(detail), "fps=%u codec=%u", entry.first, entry.second); break;
                case DiagnosticEvent::CaptureStarted:
                    std::snprintf(detail, sizeof(detail), "width=%u height=%u", entry.first, entry.second); break;
                case DiagnosticEvent::CaptureWindowAttempt:
                    std::snprintf(detail, sizeof(detail), "attempt=%u", entry.first); break;
                case DiagnosticEvent::GameTarget:
                    std::snprintf(detail, sizeof(detail), "game_pid=%u", entry.first); break;
                case DiagnosticEvent::ReplayStopped:
                    std::snprintf(detail, sizeof(detail), "state=%u", entry.first); break;
                case DiagnosticEvent::StartupStage: case DiagnosticEvent::CaptureFailed:
                    std::snprintf(detail, sizeof(detail), "stage=%u", entry.first); break;
                case DiagnosticEvent::StartFailed:
                    std::snprintf(detail, sizeof(detail), "retry_on_foreground=%u", entry.first); break;
                case DiagnosticEvent::CompressionAttempt:
                    std::snprintf(detail, sizeof(detail), "attempt=%u budget_bytes=%u", entry.first, entry.second); break;
                case DiagnosticEvent::CompressionInputBytes: case DiagnosticEvent::CompressionOutputBytes:
                    std::snprintf(detail, sizeof(detail), "bytes=%llu",
                        (static_cast<unsigned long long>(entry.first) << 32) | entry.second); break;
                case DiagnosticEvent::CompressionSource: case DiagnosticEvent::CompressionPlan:
                    std::snprintf(detail, sizeof(detail), "width=%u height=%u", entry.first, entry.second); break;
                case DiagnosticEvent::CompressionDuration: case DiagnosticEvent::CompressionFinished:
                    std::snprintf(detail, sizeof(detail), "milliseconds=%u", entry.first); break;
                case DiagnosticEvent::CompressionSourceFormat:
                    std::snprintf(detail, sizeof(detail), "codec_id=%u pixel_format=%d", entry.first, static_cast<int>(entry.second)); break;
                case DiagnosticEvent::CompressionColour:
                    std::snprintf(detail, sizeof(detail), "colour_space=%u range=%u", entry.first, entry.second); break;
                case DiagnosticEvent::CompressionRate:
                    std::snprintf(detail, sizeof(detail), "bitrate=%u limit_30_fps=%u", entry.first, entry.second); break;
                case DiagnosticEvent::CompressionDecoder:
                    std::snprintf(detail, sizeof(detail), "hardware=%u codec_id=%u", entry.first, entry.second); break;
                case DiagnosticEvent::CompressionDecodeDevice:
                    std::snprintf(detail, sizeof(detail), "d3d11va=1"); break;
                case DiagnosticEvent::CompressionAdapter:
                    std::snprintf(detail, sizeof(detail), "vendor=0x%04X device=0x%04X", entry.first, entry.second); break;
                case DiagnosticEvent::CompressionDriver:
                    std::snprintf(detail, sizeof(detail), "version=%u.%u.%u.%u", entry.first >> 16, entry.first & 65535,
                        entry.second >> 16, entry.second & 65535); break;
                case DiagnosticEvent::CompressionFrameFormat:
                    std::snprintf(detail, sizeof(detail), "decoded_format=%d transfer_format=%d",
                        static_cast<int>(entry.first), static_cast<int>(entry.second)); break;
                case DiagnosticEvent::CompressionEncoder:
                    std::snprintf(detail, sizeof(detail), "name=h264_mf hardware_requested=1 pixel_format=%d encoder_index=%u",
                        static_cast<int>(entry.first), entry.second); break;
                case DiagnosticEvent::CompressionFrames:
                    std::snprintf(detail, sizeof(detail), "decoded=%u submitted=%u", entry.first, entry.second); break;
                case DiagnosticEvent::CompressionPackets:
                    std::snprintf(detail, sizeof(detail), "encoded_video=%u muxed_video=%u", entry.first, entry.second); break;
                case DiagnosticEvent::CompressionStage: case DiagnosticEvent::CompressionError: case DiagnosticEvent::CompressionSystemError:
                    std::snprintf(detail, sizeof(detail), "stage=%s",
                        compressionStages[entry.first < compressionStages.size() ? entry.first : 0]); break;
                case DiagnosticEvent::CompressionLibraries:
                    std::snprintf(detail, sizeof(detail), "avcodec=%u.%u.%u avformat=%u.%u.%u",
                        entry.first >> 16, (entry.first >> 8) & 255, entry.first & 255,
                        entry.second >> 16, (entry.second >> 8) & 255, entry.second & 255); break;
                case DiagnosticEvent::CompressionTimeBase: case DiagnosticEvent::CompressionFrameRate:
                    std::snprintf(detail, sizeof(detail), "numerator=%u denominator=%u", entry.first, entry.second); break;
                case DiagnosticEvent::CompressionLumaSample:
                    std::snprintf(detail, sizeof(detail), "sample=%u milliseconds=%u", entry.first, entry.second); break;
                case DiagnosticEvent::CompressionDecodedLuma: case DiagnosticEvent::CompressionScaledLuma:
                    if (FAILED(entry.result)) std::snprintf(detail, sizeof(detail), "sample_unavailable=1");
                    else std::snprintf(detail, sizeof(detail), "min=%u max=%u mean=%u", entry.first >> 8, entry.first & 255, entry.second);
                    break;
                default: break;
                }
                std::snprintf(line, sizeof(line),
                    "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ %s %s%s%s%s\r\n",
                    entry.time.wYear, entry.time.wMonth, entry.time.wDay, entry.time.wHour,
                    entry.time.wMinute, entry.time.wSecond, entry.time.wMilliseconds,
                    severity, names[static_cast<std::size_t>(entry.event)], errorCode, detail[0] ? " " : "", detail);
                // Logging is best effort. A storage error never affects capture.
                if (!append(folder, line, limit)) break;
                const auto lost = dropped.exchange(0);
                if (lost) {
                    std::snprintf(line, sizeof(line), "events dropped count=%u\r\n", lost);
                    if (!append(folder, line, limit)) break;
                }
            }
        } catch (...) {} // Includes allocation and filesystem failures.
        std::lock_guard lock(mutex);
        done = true;
        finished.notify_all();
    }
};

DiagnosticLog::DiagnosticLog(std::size_t bytes) : state_(std::make_shared<State>(bytes)) {}
DiagnosticLog::~DiagnosticLog() { finish(); }

bool DiagnosticLog::configure(bool enabled, const std::filesystem::path& folder, const char* version) noexcept {
    enabled_.store(false, std::memory_order_relaxed);
    if (!enabled) return true;
    try {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping || state_->done || folder.empty()) return false;
        if (!state_->started) {
            state_->folder = folder;
            // Version comes from CMake, never from a window or a settings value.
            state_->version = std::string(version).substr(0, 64);
            std::thread([state = state_] { state->run(); }).detach();
            state_->started = true;
        }
        enabled_.store(true, std::memory_order_relaxed);
        return true;
    } catch (...) { return false; }
}

void DiagnosticLog::write(DiagnosticEvent event, HRESULT result, unsigned first, unsigned second) noexcept {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    if (static_cast<std::size_t>(event) >= names.size()) return;
    try {
        std::unique_lock lock(state_->mutex, std::try_to_lock);
        if (!lock || state_->count == state_->queue.size()) { ++state_->dropped; return; }
        if (state_->stopping || state_->done || !enabled_.load(std::memory_order_relaxed)) return;
        Entry entry{{}, event, result, first, second};
        GetSystemTime(&entry.time);
        state_->queue[(state_->head + state_->count) % state_->queue.size()] = entry;
        ++state_->count;
        state_->ready.notify_one();
    } catch (...) {}
}

bool DiagnosticLog::finish(DWORD timeoutMs) noexcept {
    enabled_.store(false, std::memory_order_relaxed);
    try {
        std::unique_lock lock(state_->mutex);
        if (!state_->started) return true;
        state_->stopping = true;
        state_->ready.notify_one();
        return state_->finished.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return state_->done; });
    } catch (...) { return false; }
}

DiagnosticLog& diagnosticLog() { static DiagnosticLog log; return log; }
} // namespace refarter
