// Opt-in hardware integration check: creates and records its own moving window.
// Requires Windows Graphics Capture, a hardware H.264 encoder and a desktop.
#include "capture/recorder.h"
#include "capture/clip_folder.h"
#include <chrono>
#include <future>
#include <iostream>

using namespace refarter::capture;
using namespace std::chrono_literals;

LRESULT CALLBACK pattern(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_TIMER) { InvalidateRect(window, nullptr, FALSE); return 0; }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        auto dc = BeginPaint(window, &paint);
        RECT rect{}; GetClientRect(window, &rect);
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        const int x = static_cast<int>((GetTickCount64() / 8) % 500);
        RECT bar{x, 20, x + 70, 320};
        FillRect(dc, &bar, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, w, l);
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 2) { std::cerr << "usage: recorder_lifecycle_check <output-folder>\n"; return 2; }
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    if (argc == 1 || std::wstring_view(argv[1]) == L"--source") {
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = pattern;
        wc.lpszClassName = L"RefarterLifecycleCheck";
        RegisterClassW(&wc);
        auto window = CreateWindowW(wc.lpszClassName, L"refarter lifecycle test pattern", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            40, 40, 640, 400, nullptr, nullptr, wc.hInstance, nullptr);
        SetTimer(window, 1, 16, nullptr);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        return 0;
    }
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(self) + L"\" --source";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) return 1;
    HWND window = nullptr;
    for (int i = 0; i < 100 && !window; ++i) {
        window = FindWindowW(L"RefarterLifecycleCheck", nullptr);
        DWORD pid = 0;
        if (window) GetWindowThreadProcessId(window, &pid);
        if (pid != child.dwProcessId) window = nullptr;
        if (!window) std::this_thread::sleep_for(20ms);
    }
    int result = 0;
    try {
        const auto folder = std::filesystem::absolute(argv[1]);
        if (auto error = prepareClipFolder(folder)) throw std::runtime_error("clip folder unavailable");
        VideoSettings video{Resolution::P720, 30, 8, 5};
        // Only operate on the generated child: pending startup must cancel
        // promptly, then a fresh attempt must use its restored client size.
        ShowWindowAsync(window, SW_MINIMIZE);
        for (int i = 0; i < 100 && !IsIconic(window); ++i) std::this_thread::sleep_for(20ms);
        if (!IsIconic(window)) throw std::runtime_error("test source did not minimise");
        winrt::handle cancel{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (!cancel) throw std::runtime_error("cannot create startup cancellation event");
        AudioSettings silent;
        silent.desktop = {};
        Recorder pending;
        auto begin = [&](HANDLE cancellation) {
            return std::async(std::launch::async, [&, cancellation] {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
                auto error = pending.start({window, nullptr}, video, silent, {}, {}, cancellation);
                winrt::uninit_apartment();
                return error;
            });
        };
        auto cancelled = begin(cancel.get());
        if (cancelled.wait_for(500ms) != std::future_status::timeout)
            throw std::runtime_error("minimised startup did not wait");
        const auto cancelledAt = std::chrono::steady_clock::now();
        SetEvent(cancel.get());
        if (cancelled.wait_for(2s) != std::future_status::ready ||
            cancelled.get() != std::optional<std::wstring>{L"starting replay cancelled"} ||
            pending.state() != RecorderState::Off)
            throw std::runtime_error("pending startup did not cancel promptly");
        std::wcout << L"startup cancellation: " << std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - cancelledAt).count() << L" ms\n";
        auto restored = begin(nullptr);
        if (restored.wait_for(500ms) != std::future_status::timeout)
            throw std::runtime_error("fresh minimised startup did not wait");
        ShowWindowAsync(window, SW_SHOWNOACTIVATE);
        if (restored.wait_for(5s) != std::future_status::ready || restored.get())
            throw std::runtime_error("restored startup failed");
        RECT client{};
        GetClientRect(window, &client);
        const auto expected = outputSize(video.resolution, {client.right, client.bottom});
        if (pending.outputSize().width != expected.width || pending.outputSize().height != expected.height)
            throw std::runtime_error("restored startup used placeholder dimensions");
        std::this_thread::sleep_for(1s);
        if (!pending.stats().framesEncoded) throw std::runtime_error("restored startup encoded no frames");
        pending.stop();
        std::wcout << L"PASS: pending startup cancels, fresh restored startup uses client dimensions and encodes\n";
        AudioSettings audio; // Desktop only; never opens a microphone.
        std::promise<void> stopped;
        auto stoppedFuture = stopped.get_future();
        std::promise<std::optional<std::wstring>> saved;
        auto savedFuture = saved.get_future();
        Recorder recorder;
        if (auto error = recorder.start({window, nullptr}, video, audio, [&] { stopped.set_value(); })) {
            std::wcerr << *error << L'\n';
            throw std::runtime_error("capture did not start");
        }
        std::this_thread::sleep_for(3s);
        const auto running = recorder.stats();
        if (!running.framesEncoded) throw std::runtime_error("no video packets encoded");
        PostMessageW(window, WM_CLOSE, 0, 0);
        if (stoppedFuture.wait_for(10s) != std::future_status::ready) throw std::runtime_error("automatic stop timed out");
        auto before = recorder.stats();
        if (recorder.state() != RecorderState::Stopped || before.desktopAudioActive || before.micAudioActive)
            throw std::runtime_error("producers remained active after target closure");
        std::this_thread::sleep_for(1s);
        auto after = recorder.stats();
        if (after.framesCaptured != before.framesCaptured || after.framesEncoded != before.framesEncoded ||
            after.desktopAudioSeconds != before.desktopAudioSeconds || after.bufferedBytes != before.bufferedBytes)
            throw std::runtime_error("retained replay changed after automatic stop");
        const auto path = nextClipPath(folder, L"lifecycle");
        // A file can appear after name selection but before publication. Never replace it; report the numbered output.
        const HANDLE occupied = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        if (occupied == INVALID_HANDLE_VALUE) throw std::runtime_error("could not create collision fixture");
        DWORD written = 0;
        const char marker = 'x';
        const bool marked = WriteFile(occupied, &marker, 1, &written, nullptr) && written == 1;
        CloseHandle(occupied);
        if (!marked) throw std::runtime_error("could not mark collision fixture");
        std::filesystem::path published;
        if (auto error = recorder.saveClip(path, [&](auto error, const std::filesystem::path& output) { published = output; saved.set_value(error); }))
            throw std::runtime_error("retained replay save rejected");
        if (savedFuture.wait_for(20s) != std::future_status::ready || savedFuture.get())
            throw std::runtime_error("retained replay save failed");
        if (published.empty() || published == path || !std::filesystem::exists(published) ||
            std::filesystem::file_size(path) != 1 || std::filesystem::file_size(published) <= 1)
            throw std::runtime_error("collision did not preserve the original and report the actual saved path");
        std::filesystem::remove(path); // Only the one-byte fixture, never a clip.
        recorder.stop();
        if (recorder.state() != RecorderState::Off || recorder.stats().bufferedBytes)
            throw std::runtime_error("explicit stop did not release replay");
        std::wcout << L"PASS: target close releases producers, freezes replay, saves after closure, then clears\n" << published << L'\n';
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        result = 1;
    }
    if (IsWindow(window)) PostMessageW(window, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(child.hProcess, 3000) == WAIT_TIMEOUT) TerminateProcess(child.hProcess, 1);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    winrt::uninit_apartment();
    return result;
}
