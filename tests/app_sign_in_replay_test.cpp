// Sign-in replay (docs/startup.md) through the real App: normal and --startup launches, each kind of source, busy,
// asleep and failed starts, the single retry, what cancels it, and cancelling while the sign-in start is in flight.
//
// As in app_game_settings_test: the App's window and controls are created hidden and never shown, its files live in a
// temporary folder (localAppDataOverride), a stand-in recorder records the starts and the test answers through the
// App's own notify. The Windows startup entry is read (WM_CREATE's refreshStartup) but never written: the test then
// overwrites what was read, setting "start with windows" on as for this copy's own entry. The 15 s retry timer is never
// waited for: the test sends its WM_TIMER, and sees whether one is pending by killing it and setting it again, too far
// off to fire during the test.
#include "app_internal.h"
#include "local_file.h"
#include "message_box.h"
#include "text.h"
#include "window_target.h"

#include <objidl.h>
#include <gdiplus.h>
#include <fstream>
#include <iostream>

namespace refarter {
namespace {
int failures = 0;
void check(bool passes, const std::string& name) {
    if (passes) return;
    std::cerr << "  FAILED: " << name << '\n';
    ++failures;
}

void pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

// Records the starts; never starts a worker, so close() answers Closed at once.
struct FakeRecorder final : RecordingController {
    std::vector<RecordingTarget> starts;
    Notify notify;
    explicit FakeRecorder(Notify app) : RecordingController(app), notify(std::move(app)) {}
    void start(RecordingTarget target, capture::VideoSettings, capture::AudioSettings, std::filesystem::path) override {
        starts.push_back(std::move(target));
    }
    void stop() override {}
    void pause() override {}
    void save(std::filesystem::path, std::wstring, std::int64_t) override {}
    void screenshot(std::filesystem::path, std::wstring) override {}
    void folder(std::filesystem::path) override {}
    void probeCodecs() override {}
};

void menuItems(HMENU menu, std::vector<std::pair<UINT, std::wstring>>& items) {
    for (int i = 0, count = GetMenuItemCount(menu); i < count; ++i) {
        wchar_t text[256]{};
        GetMenuStringW(menu, static_cast<UINT>(i), text, static_cast<int>(std::size(text)), MF_BYPOSITION);
        items.emplace_back(GetMenuItemID(menu, i), text);
        if (const HMENU child = GetSubMenu(menu, i)) menuItems(child, items);
    }
}

UINT itemStarting(HMENU menu, const std::wstring& prefix) {
    std::vector<std::pair<UINT, std::wstring>> items;
    menuItems(menu, items);
    for (const auto& [id, text] : items)
        if (id != 0 && id != static_cast<UINT>(-1) && lower(text).starts_with(lower(prefix))) return id;
    return 0;
}

bool has(const std::wstring& text, const wchar_t* part) { return text.find(part) != std::wstring::npos; }

const std::wstring missingMonitor = L"\\\\.\\DISPLAY99";
} // namespace

struct AppTestAccess {
    struct Setup {
        bool startup = true;      // A --startup launch; false: an ordinary one.
        bool signInReplay = true; // [window] start_replay.
        std::wstring monitor;     // [recording] source_monitor; empty: no monitor remembered.
    };

    std::filesystem::path folder;
    std::unique_ptr<App> app;
    FakeRecorder* recorder = nullptr; // Owned by the App.
    std::vector<HWND> windows;        // Stand-ins for game windows.
    std::function<UINT(HMENU)> menu;
    bool ok = false;

    // `before` runs after the App is created and before its first messages (the posted sign-in start among them).
    AppTestAccess(const Setup& setup, const std::function<void(AppTestAccess&)>& before) {
        static int count = 0;
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        folder = std::filesystem::path(temp) /
            (L"refarter-sign-in-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++count));
        std::error_code error;
        std::filesystem::remove_all(folder, error);
        std::filesystem::create_directories(folder / L"clips", error);
        localAppDataOverride() = folder;
        writeIni(setup);
        app = std::make_unique<App>();
        app->makeRecorder_ = [this](RecordingController::Notify notify) {
            auto fake = std::make_unique<FakeRecorder>(std::move(notify));
            recorder = fake.get();
            return std::unique_ptr<RecordingController>(std::move(fake));
        };
        app->answerMenu_ = [this](HMENU shown) -> UINT { return menu ? menu(shown) : 0u; };
        if (!app->create(GetModuleHandleW(nullptr), setup.startup) || !recorder) return;
        // As refreshStartup reads for this copy's own entry, whatever the developer's machine has registered.
        app->state_.startWithWindows = true;
        app->startupReadFailed_ = app->startupOtherCopy_ = false;
        app->refreshControls();
        if (before) before(*this);
        pump();
        ok = true;
    }

    ~AppTestAccess() {
        if (app) {
            app->answerMenu_ = nullptr;
            app->asleep_ = app->busy_ = false; // Quit as from an idle App.
            if (IsWindow(app->window_)) DestroyWindow(app->window_);
            pump();
            app.reset();
            pump();
        }
        UnregisterClassW(L"RefarterDesktopShell", GetModuleHandleW(nullptr));
        for (const HWND window : windows) DestroyWindow(window);
        localAppDataOverride().clear();
        std::error_code error;
        std::filesystem::remove_all(folder, error);
    }

    void writeIni(const Setup& setup) const {
        const auto shortcut = [](UINT key) { return hotkeyText({MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN, key}); };
        const std::wstring ini =
            L"[recording]\r\nauto_capture=off\r\nauto_capture_prompt=off\r\nsource_monitor=" + setup.monitor + L"\r\n"
            L"[video]\r\ncodec=h264\r\n"
            L"[audio]\r\ndesktop_device=off\r\nmicrophone_device=off\r\nchime_volume=0\r\n"
            L"[storage]\r\nclip_folder=" + (folder / L"clips").wstring() + L"\r\n"
            L"[window]\r\nclose_to_tray=off\r\nstart_replay=" + (setup.signInReplay ? L"on" : L"off") + L"\r\n"
            L"[updates]\r\ncheck=off\r\n"
            L"[shortcuts]\r\nsave_replay=" + shortcut(VK_F23) + L"\r\nscreenshot=" + shortcut(VK_F24) + L"\r\n";
        std::ofstream output(folder / L"refarter.ini", std::ios::binary);
        output.write("\xff\xfe", 2);
        output.write(reinterpret_cast<const char*>(ini.data()), static_cast<std::streamsize>(ini.size() * sizeof(wchar_t)));
    }

    // A hidden popup stands in for a game's window, picked as chooseSource picks one.
    void chooseGame() {
        const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"X Game", WS_POPUP,
            0, 0, 1280, 720, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        windows.push_back(window);
        RecordingTarget target;
        target.source.window = window;
        target.pid = GetCurrentProcessId();
        target.name = L"x.exe";
        target.title = target.displayName = L"X Game";
        target.path = L"C:\\Games\\X\\x.exe";
        app->target_ = std::move(target);
        app->setSourceMonitor({});
        app->followSource();
        pump();
    }

    bool click(int id) {
        const HWND button = app->control(id);
        if (!IsWindowEnabled(button)) return false;
        SendMessageW(app->window_, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(button));
        pump();
        return true;
    }
    bool pick(int button, const std::wstring& prefix) {
        bool found = false;
        menu = [&](HMENU shown) {
            const UINT item = itemStarting(shown, prefix);
            found = item != 0;
            return item;
        };
        const bool clicked = click(button);
        menu = nullptr;
        return clicked && found;
    }

    // Whether the one retry is waiting: KillTimer says if the timer existed. If so it is set again, but as far off as
    // allowed, so the real one can never fire inside a later pump(); only retry() delivers it.
    bool retryPending() const {
        if (!KillTimer(app->window_, signInReplayTimer)) return false;
        SetTimer(app->window_, signInReplayTimer, USER_TIMER_MAXIMUM, nullptr);
        return true;
    }
    // The retry's time has come.
    void retry() {
        SendMessageW(app->window_, WM_TIMER, signInReplayTimer, 0);
        pump();
    }
    // The recorder answers the latest start, as from its worker.
    void answer(bool success) {
        RecordingEvent event{RecordingEvent::Kind::Started};
        event.success = success;
        event.state = success ? capture::RecorderState::Buffering : capture::RecorderState::Off;
        event.message = success ? L"Buffering · 1920 × 1080 · 60 fps · h.264" : L"Capture could not start";
        recorder->notify(std::move(event));
        pump();
    }
    size_t starts() const { return recorder->starts.size(); }
    const std::wstring& status() const { return app->status_; }
    bool inFlight() const { return app->signInReplay_; } // The start now under way is the sign-in start.
    bool dialogs() const { return !app->queuedErrors_.empty(); }
    bool buffering() const { return app->recording_ == capture::RecorderState::Buffering; }

    static int runAll();
};

int AppTestAccess::runAll() {
    const auto displays = displayMonitors();
    if (displays.empty()) {
        std::cout << "SKIP: no display monitor, which the sign-in replay test needs\n";
        return 77; // SKIP_RETURN_CODE in CMakeLists.txt.
    }
    const HMONITOR first = displays.front();
    const std::wstring remembered = monitorDevice(first);
    const auto run = [](const char* name, const Setup& setup, const std::function<void(AppTestAccess&)>& before, auto body) {
        const int was = failures;
        {
            AppTestAccess h(setup, before);
            if (h.ok) body(h);
            else check(false, std::string(name) + ": the app could not be created");
        }
        std::cout << (failures == was ? "pass  " : "FAIL  ") << name << '\n';
    };
    const std::function<void(AppTestAccess&)> nothing;

    // Launches.
    run("S1 an ordinary launch never starts replay", Setup{.startup = false, .monitor = remembered}, nothing, [](AppTestAccess& h) {
        check(h.starts() == 0, "S1 nothing started");
        check(!h.retryPending(), "S1 no retry");
    });
    run("S2 a --startup launch with start replay off starts nothing", Setup{.signInReplay = false, .monitor = remembered}, nothing,
        [](AppTestAccess& h) {
        check(h.starts() == 0, "S2 nothing started");
        check(!h.retryPending(), "S2 no retry");
    });
    run("S3 --startup starts the remembered monitor quietly", Setup{.monitor = remembered}, nothing, [&](AppTestAccess& h) {
        check(h.starts() == 1, "S3 one start");
        check(h.starts() == 1 && h.recorder->starts[0].source.monitor == first && !h.recorder->starts[0].source.window,
              "S3 the remembered monitor, whole");
        check(h.inFlight(), "S3 marked as the sign-in start");
        h.answer(true);
        check(h.buffering(), "S3 replay on");
        check(!h.inFlight(), "S3 the marker ends with its Started event");
        check(!h.retryPending() && !h.dialogs(), "S3 no retry, no dialog");
    });

    // Sources.
    run("S4 no source: nothing starts, no retry", Setup{}, nothing, [](AppTestAccess& h) {
        check(h.starts() == 0, "S4 nothing started");
        check(!h.retryPending(), "S4 no retry");
        check(!has(h.status(), L"sign-in"), "S4 nothing to say");
    });
    run("S5 a game as the source: nothing starts", Setup{.monitor = remembered},
        [](AppTestAccess& h) { h.chooseGame(); }, [](AppTestAccess& h) {
        check(h.starts() == 0, "S5 nothing started");
        check(!h.retryPending(), "S5 no retry");
    });
    run("S6 a missing monitor: exactly one retry, then a status", Setup{.monitor = missingMonitor}, nothing, [](AppTestAccess& h) {
        check(h.starts() == 0, "S6 nothing started");
        check(has(h.status(), L"not connected · trying again shortly"), "S6 the reason and the retry");
        check(h.retryPending(), "S6 retry waiting");
        h.retry();
        check(h.starts() == 0, "S6 still nothing started");
        check(has(h.status(), L"not connected · enable replay"), "S6 gives up, keeping the reason");
        check(!h.retryPending(), "S6 no second retry");
        check(!h.dialogs(), "S6 never a dialog");
    });
    run("S7 invalid replay settings: a status at once, no retry", Setup{.monitor = remembered},
        [](AppTestAccess& h) { h.app->state_.replayInputValid = false; }, [](AppTestAccess& h) {
        check(h.starts() == 0, "S7 nothing started");
        check(has(h.status(), L"check the replay settings"), "S7 says why");
        check(!h.retryPending(), "S7 no retry");
        check(!h.inFlight(), "S7 no marker left");
    });

    // Failed, busy and asleep starts.
    run("S8 a failed start retries once, quietly", Setup{.monitor = remembered}, nothing, [](AppTestAccess& h) {
        h.answer(false);
        check(has(h.status(), L"trying again shortly"), "S8 says it will try again");
        check(h.retryPending() && !h.inFlight(), "S8 retry waiting, marker ended");
        check(!h.dialogs(), "S8 no dialog");
        h.retry();
        check(h.starts() == 2 && h.inFlight(), "S8 the retry starts once more, as the sign-in start");
        h.answer(false);
        check(has(h.status(), L"enable replay"), "S8 gives up");
        check(!h.retryPending(), "S8 no second retry");
        check(!h.dialogs(), "S8 still no dialog");
    });
    run("S9 busy at sign-in: the retry starts once free", Setup{.monitor = remembered},
        [](AppTestAccess& h) { h.app->busy_ = true; }, [](AppTestAccess& h) {
        check(h.starts() == 0, "S9 nothing started while busy");
        check(has(h.status(), L"replay could not start at sign-in · trying again shortly"), "S9 status");
        check(h.retryPending(), "S9 retry waiting");
        h.app->busy_ = false;
        h.retry();
        check(h.starts() == 1 && h.inFlight(), "S9 the retry starts");
        h.answer(true);
        check(h.buffering() && !h.retryPending() && !h.dialogs(), "S9 replay on, nothing left");
    });
    run("S10 asleep for both tries: gives up", Setup{.monitor = remembered},
        [](AppTestAccess& h) { h.app->asleep_ = true; }, [](AppTestAccess& h) {
        check(h.starts() == 0 && h.retryPending(), "S10 nothing started, retry waiting");
        h.retry();
        check(h.starts() == 0, "S10 still nothing started");
        check(has(h.status(), L"enable replay") && !h.retryPending(), "S10 gives up after one retry");
    });

    // What cancels a waiting retry.
    run("S11 choosing a monitor cancels the retry", Setup{.monitor = missingMonitor}, nothing, [&](AppTestAccess& h) {
        check(h.retryPending(), "S11 retry waiting");
        check(h.pick(SelectProcess, L"monitor 1"), "S11 monitor 1 picked");
        check(h.app->sourceMonitor_ == remembered, "S11 the new monitor is remembered");
        check(!h.retryPending() && h.starts() == 0, "S11 no retry, nothing started");
    });
    run("S12 choosing a game cancels the retry", Setup{.monitor = remembered},
        [](AppTestAccess& h) { h.app->busy_ = true; }, [](AppTestAccess& h) {
        check(h.retryPending(), "S12 retry waiting");
        h.app->busy_ = false;
        h.chooseGame();
        check(!h.retryPending() && h.starts() == 0, "S12 no retry, nothing started");
    });
    run("S13 switching start replay off cancels the retry", Setup{.monitor = missingMonitor}, nothing, [](AppTestAccess& h) {
        check(h.retryPending(), "S13 retry waiting");
        check(h.click(SignInReplay) && !h.app->state_.signInReplay, "S13 switched off");
        check(!h.retryPending(), "S13 no retry");
    });
    run("S14 starting replay by hand cancels the retry", Setup{.monitor = remembered},
        [](AppTestAccess& h) { h.app->busy_ = true; }, [](AppTestAccess& h) {
        check(h.retryPending(), "S14 retry waiting");
        h.app->busy_ = false;
        h.app->refreshControls(); // The start button follows busy_.
        check(h.click(StartReplay) && h.starts() == 1, "S14 the user's start");
        check(!h.retryPending() && !h.inFlight(), "S14 no retry, not the sign-in start");
        h.answer(false);
        check(h.dialogs(), "S14 the user's own failed start is a dialog");
        check(!h.retryPending(), "S14 and never retried");
    });
    run("S15 another automatic start first cancels the retry", Setup{.monitor = remembered},
        [](AppTestAccess& h) { h.app->busy_ = true; }, [](AppTestAccess& h) {
        check(h.retryPending(), "S15 retry waiting");
        h.app->busy_ = false;
        check(h.app->startReplay(false) && !h.inFlight(), "S15 auto-capture's start");
        h.answer(true);
        check(!h.retryPending(), "S15 no retry");
    });

    // Cancelling while the sign-in start is in flight.
    run("S16 start replay switched off mid-start: a failure stays quiet, no retry", Setup{.monitor = remembered}, nothing,
        [](AppTestAccess& h) {
        check(h.starts() == 1 && h.inFlight(), "S16 in flight");
        check(h.click(SignInReplay) && !h.app->state_.signInReplay, "S16 switched off");
        h.answer(false);
        check(!h.retryPending(), "S16 no retry");
        check(!h.dialogs(), "S16 still a status, not a dialog");
        check(has(h.status(), L"enable replay"), "S16 says replay is off");
        check(!h.inFlight(), "S16 marker ended");
    });
    run("S17 start replay switched off mid-start: a success stays on", Setup{.monitor = remembered}, nothing, [](AppTestAccess& h) {
        check(h.click(SignInReplay), "S17 switched off");
        h.answer(true);
        check(h.buffering() && !h.retryPending() && !h.dialogs(), "S17 replay on, nothing left");
    });
    run("S18 an already-queued retry stays cancelled", Setup{.monitor = remembered},
        [](AppTestAccess& h) { h.app->busy_ = true; }, [&](AppTestAccess& h) {
        check(h.retryPending() && h.starts() == 0, "S18 retry waiting");
        h.app->busy_ = false;
        h.app->refreshControls(); // As the real busy operation's completion would do.
        PostMessageW(h.app->window_, WM_TIMER, signInReplayTimer, 0);
        check(h.pick(SelectProcess, L"monitor 1"), "S18 source selection cancels before the queued timer is dispatched");
        check(h.starts() == 0 && !h.retryPending(), "S18 stale message did not start capture");
    });
    run("S19 an already-queued initial start stays cancelled", Setup{.monitor = remembered},
        [&](AppTestAccess& h) { h.app->setSourceMonitor(remembered); }, [](AppTestAccess& h) {
        check(h.starts() == 0 && !h.retryPending(), "S19 cancelled initial notification is ignored");
    });
    run("S20 a consumed retry cannot start again", Setup{.monitor = remembered}, nothing, [](AppTestAccess& h) {
        h.answer(false); h.retry(); h.answer(false);
        check(h.starts() == 2 && !h.retryPending(), "S20 both attempts ended");
        h.retry();
        check(h.starts() == 2 && !h.retryPending(), "S20 stale retry cannot make a third attempt");
    });
    return failures;
}
} // namespace refarter

int main() {
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // As Refarter runs.
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
        std::cerr << "gdi+ could not start\n";
        return 1;
    }
    refarter::suppressDialogs(true); // Any dialog answers at once: nothing here may wait for a click.
    const int result = refarter::AppTestAccess::runAll();
    refarter::suppressDialogs(false);
    Gdiplus::GdiplusShutdown(token);
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
