// No GPU work: only cancellation, a missing source, the already-small shortcut
// and an invalid padded MP4 that fails before decoder/encoder creation.
#include "diagnostic_log.h"
#include "compression_job.h"
#include "player/clip_export.h"
#include "player/export_luma.h"
#include <array>
#include <fstream>
#include <iostream>

int main() {
    using namespace refarter;
    using refarter::player::compressClip;
    int failures = 0;
    const auto check = [&](bool okay, const char* text) { if (!okay) { std::cerr << text << '\n'; ++failures; } };
    {
        AVFrame frame{};
        std::array<std::uint8_t, 8> plane{16, 235, 99, 99, 16, 235, 99, 99};
        frame.format = AV_PIX_FMT_NV12; frame.width = 2; frame.height = 2;
        frame.linesize[0] = 4; frame.data[0] = plane.data();
        const auto luma = player::sampleExportLuma(frame);
        check(luma && luma->minimum == 16 && luma->maximum == 235 && luma->mean == 125,
            "luma summary excludes padded stride bytes");
        frame.data[0] += 4; frame.linesize[0] = -4;
        const auto flipped = player::sampleExportLuma(frame);
        check(flipped && flipped->mean == 125, "negative row stride is supported");
        frame.linesize[0] = 1;
        check(!player::sampleExportLuma(frame), "undersized row stride is rejected");
        std::array<std::uint8_t, 4> tenBit{64, 0, 172, 3}; // 64, 940 in low bits.
        frame.data[0] = tenBit.data(); frame.linesize[0] = 4; frame.height = 1;
        frame.format = AV_PIX_FMT_YUV420P10LE;
        const auto lowBits = player::sampleExportLuma(frame);
        check(lowBits && lowBits->minimum == 16 && lowBits->maximum == 235, "planar 10-bit luma is normalised");
        tenBit = {0, 16, 0, 235}; frame.format = AV_PIX_FMT_P010LE;
        const auto highBits = player::sampleExportLuma(frame);
        check(highBits && highBits->minimum == 16 && highBits->maximum == 235, "P010 high-bit luma is normalised");
        frame.format = AV_PIX_FMT_D3D11;
        check(!player::sampleExportLuma(frame), "GPU handles are never treated as CPU pixels");
    }
    const auto root = std::filesystem::temp_directory_path() /
        (L"refarter-compression-log-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    const auto small = root / L"private-source-do-not-log-small.mp4";
    { std::ofstream(small) << "small fixture"; }
    const auto invalid = root / L"private-source-do-not-log-invalid.mp4";
    const HANDLE fixture = CreateFileW(invalid.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fixture == INVALID_HANDLE_VALUE) {
        std::cerr << "could not create fixture\n";
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored); // Do not leak the test-owned temporary tree.
        return 1;
    }
    LARGE_INTEGER size{}; size.QuadPart = 20'000'001;
    const bool padded = SetFilePointerEx(fixture, size, nullptr, FILE_BEGIN) && SetEndOfFile(fixture);
    CloseHandle(fixture);
    check(padded, "invalid fixture exceeds the skip threshold");
    auto& log = diagnosticLog();
    check(log.configure(true, root / L"logs", "compression-test"), "logging enabled");
    std::atomic<float> progress{0};
    std::atomic<bool> cancel{true};
    check(compressClip(small, progress, cancel).cancelled, "pre-cancelled job does not export");
    cancel.store(false);
    check(compressClip(small, progress, cancel).alreadyFits, "small source is skipped");
    check(!compressClip(root / L"private-source-do-not-log-missing.mp4", progress, cancel).error.empty(), "missing source fails safely");
    check(!compressClip(invalid, progress, cancel).error.empty(), "invalid MP4 fails before GPU activation");
    {
        CompressionJob job;
        const auto wait = [&]() {
            std::optional<player::ExportResult> result;
            // Generous: a loaded machine must not fail the test; a hung job still ends it after 30 s.
            const auto deadline = GetTickCount64() + 30000;
            while (!(result = job.poll()) && GetTickCount64() < deadline) Sleep(1);
            return result;
        };
        check(job.start(small, true), "owned small trim starts");
        const auto skipped = wait();
        check(skipped && skipped->alreadyFits && std::filesystem::exists(small),
            "already-fit trim remains the final result, never cleaned up");
        check(job.start(invalid, true), "owned invalid trim starts");
        const auto failed = wait();
        check(failed && !failed->error.empty() && std::filesystem::exists(invalid),
            "failed export retains the raw trim for recovery");
    }
    check(log.finish(2000), "log writer finishes");
    std::ifstream stream(root / L"logs" / L"session.log", std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(stream), {}};
    check(text.find("compression cancelled") != std::string::npos, "cancellation logged");
    check(text.find("compression skipped") != std::string::npos, "already-small result logged");
    check(text.find("compression system error hr=0x80070002 stage=lock_source") != std::string::npos,
        "missing source retains Windows code and stage");
    check(text.find("ffmpeg_error=") != std::string::npos && text.find("stage=open_input") != std::string::npos,
        "bad input retains FFmpeg error and stage");
    check(text.find("private-source-do-not-log") == std::string::npos, "personal filenames never enter the session log");
    check(text.find("compression encoder") == std::string::npos && text.find("compression decode device") == std::string::npos,
        "failure fixtures never reach GPU setup");
    for (const auto& file : std::filesystem::directory_iterator(root))
        check(file.path().extension() != L".part", "failed job removes its partial output");
    stream.close();
    std::filesystem::remove_all(root); // Exact unique test-owned temporary fixture tree.
    return failures ? 1 : 0;
}
