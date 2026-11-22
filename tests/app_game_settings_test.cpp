// The per-game recording settings (docs/game-settings.md) through the real App, one test per row of the per-game
// state table (T1-T25), each named after its row.
//
// The App's window and controls are created hidden and are never shown, focused or activated. Every file under
// %LOCALAPPDATA%\Refarter goes to a temporary folder of the test's own (localAppDataOverride), whose refarter.ini turns
// audio, auto-capture, chimes and the update check off, puts the clip folder in the same temporary folder and moves
// the shortcuts to combinations nobody uses. A stand-in recorder (App::makeRecorder_) records the commands and the
// test answers through the App's own notify, so events take the real path (a posted message, then receiveEvents).
// Menus are answered by the test (App::answerMenu_) and dialogs are suppressed, so nothing waits for a click.
#include "app_internal.h"
#include "local_file.h"
#include "message_box.h"
#include "text.h"
#include "startup.h"
#include "window_target.h"

#include <objidl.h>
#include <gdiplus.h>
#include <fstream>
#include <iostream>
#include <iterator>

namespace refarter {
namespace {
int failures = 0;
void check(bool passes, const std::string& name) {
    if (passes) return;
    std::cerr << "  FAILED: " << name << '\n';
    ++failures;
}

const std::wstring xPath = L"C:\\Games\\X\\x.exe", yPath = L"C:\\Games\\Y\\y.exe", zPath = L"C:\\Games\\Z\\z.exe",
                   aPath = L"C:\\Games\\A\\a.exe";

// Runs what is posted: recorder events, follow-ups. A WM_QUIT from a destroyed window is dropped.
void pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

// Records what the App asks of its recorder. It never starts a worker, so close() answers Closed at once, as the real
// one does when nothing ever ran.
struct FakeRecorder final : RecordingController {
    struct Start { RecordingTarget target; capture::VideoSettings video; };
    std::vector<Start> starts;
    int stops = 0, pauses = 0, probes = 0;
    Notify notify; // The App's own: what the test answers through.
    explicit FakeRecorder(Notify app) : RecordingController(app), notify(std::move(app)) {}
    void start(RecordingTarget target, capture::VideoSettings video, capture::AudioSettings, std::filesystem::path) override {
        starts.push_back({std::move(target), video});
    }
    void stop() override { ++stops; }
    // Answered at once, as the real recorder answers a pause with nothing kept: a plain stop marked `sleeping`.
    void pause() override {
        ++pauses;
        RecordingEvent event{RecordingEvent::Kind::Stopped};
        event.sleeping = true;
        event.message = L"replay paused for sleep";
        notify(std::move(event));
    }
    void save(std::filesystem::path, std::wstring, std::int64_t) override {}
    void screenshot(std::filesystem::path, std::wstring) override {}
    void folder(std::filesystem::path) override {}
    void probeCodecs() override { ++probes; }
};

void menuItems(HMENU menu, std::vector<std::pair<UINT, std::wstring>>& items) {
    for (int i = 0, count = GetMenuItemCount(menu); i < count; ++i) {
        wchar_t text[256]{};
        GetMenuStringW(menu, static_cast<UINT>(i), text, static_cast<int>(std::size(text)), MF_BYPOSITION);
        items.emplace_back(GetMenuItemID(menu, i), text);
        if (const HMENU child = GetSubMenu(menu, i)) menuItems(child, items);
    }
}

// The id of the first item (submenus too) whose text starts with `prefix`, ignoring case; 0 (dismissed) if none.
UINT itemStarting(HMENU menu, const std::wstring& prefix) {
    std::vector<std::pair<UINT, std::wstring>> items;
    menuItems(menu, items);
    for (const auto& [id, text] : items)
        if (id != 0 && id != static_cast<UINT>(-1) && lower(text).starts_with(lower(prefix))) return id;
    return 0;
}

std::string fileBytes(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

capture::Size primaryScreen() {
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &info);
    return {info.rcMonitor.right - info.rcMonitor.left, info.rcMonitor.bottom - info.rcMonitor.top};
}

GameSettingsList list(std::initializer_list<GameSettingsEntry> entries) {
    GameSettingsList games;
    for (const auto& entry : entries) games.set(entry.path, entry.label, entry.video);
    return games;
}
GameSettingsEntry gameX(GameVideo video) { return {xPath, L"X Game", video}; }
GameSettingsEntry gameY(GameVideo video) { return {yPath, L"Y Game", video}; }

// This PC's check: h.264 records, hevc does not.
std::vector<capture::CodecSupport> hevcRefused() {
    return {{capture::Codec::H264, true, true, L"", L""}, {capture::Codec::HEVC, false, false, L"", L"no hevc encoder"}};
}
} // namespace

// One App with its files in a temporary folder; friends with App, so the tests below reach its private state.
struct AppTestAccess {
    // All games' values in refarter.ini, and game_settings.dat (written only if it has games, or as raw text).
    struct Setup {
        bool fresh = false;
        bool welcomePending = false;
        bool signInReplay = false;
        std::wstring sourceMonitor;
        int seconds = 180;
        capture::Quality quality = capture::Quality::Balanced;
        int bitrate = 50;
        capture::Resolution resolution = capture::Resolution::Native;
        GameSettingsList games;
        std::optional<std::wstring> gameFileText; // A foreign or newer file, instead of `games`.
        std::optional<std::wstring> gameNamesText; // game_names.dat as raw text.
    };

    std::filesystem::path folder;
    std::wstring startupTestKey;
    std::unique_ptr<App> app;
    FakeRecorder* recorder = nullptr; // Owned by the App.
    std::vector<HWND> windows;        // Stand-ins for game windows.
    HANDLE lock = INVALID_HANDLE_VALUE;
    std::function<UINT(HMENU)> menu;  // Answers the next menu the App opens; empty: dismissed.
    bool ok = false;
    bool fresh = false;

    explicit AppTestAccess(const Setup& setup) {
        static int count = 0;
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        folder = std::filesystem::path(temp) /
            (L"refarter-app-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++count));
        std::error_code error;
        std::filesystem::remove_all(folder, error);
        std::filesystem::create_directories(folder / L"clips", error);
        startupTestKey = L"Software\\RefarterTests\\app-settings-" + folder.filename().wstring() + L"-" + std::to_wstring(GetTickCount64());
        localAppDataOverride() = folder;
        writeIni(setup);
        if (setup.signInReplay) writeStartup(startupCommand(executablePath()), startupTestKey.c_str());
        if (setup.gameFileText) writeUtf16FileAtomic(gameSettingsPath(), *setup.gameFileText, 1 << 20, L".test-");
        else if (!setup.games.games.empty()) writeGameSettings(gameSettingsPath(), setup.games);
        if (setup.gameNamesText) writeUtf16FileAtomic(gameNamesPath(), *setup.gameNamesText, 1 << 20, L".test-");
        fresh = setup.fresh;
        if (fresh) std::filesystem::remove(configPath());
        ok = open();
    }

    // Creates the App over the folder's files, as Refarter starting.
    bool open() {
        app = std::make_unique<App>();
        app->startupKeyOverride_ = startupTestKey;
        app->makeRecorder_ = [this](RecordingController::Notify notify) {
            if (fresh) {
                // A fresh file has no embedded defaults in this test executable. Keep even that path isolated.
                app->clipFolder_ = folder / L"clips";
                app->audio_.desktop = capture::AudioDevice{};
                app->audio_.microphone = capture::AudioDevice{};
                app->state_.updateCheck = UpdateChoice::Off;
                app->autoCaptureOn_ = app->autoCapturePrompt_ = false;
                // WM_CREATE reads the shortcuts from the file just after this: never register the real defaults,
                // which a running Refarter may hold.
                app->config_.set(L"shortcuts", L"save_replay", hotkeyText({MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN, VK_F23}));
                app->config_.set(L"shortcuts", L"screenshot", hotkeyText({MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN, VK_F24}));
            }
            auto fake = std::make_unique<FakeRecorder>(std::move(notify));
            recorder = fake.get();
            return std::unique_ptr<RecordingController>(std::move(fake));
        };
        app->answerMenu_ = [this](HMENU shown) -> UINT { return menu ? menu(shown) : 0u; };
        // As a sign-in start: the library scan waits for the window to open, which it never does here.
        if (!app->create(GetModuleHandleW(nullptr), true) || !recorder) return false;
        app->state_.page = Page::Recording; // What navigate() sets, without moving focus (that would activate the window).
        app->layout();
        pump();
        return true;
    }

    void close() {
        if (app) {
            app->answerMenu_ = nullptr;
            app->answerName_ = nullptr;
            if (IsWindow(app->window_)) DestroyWindow(app->window_);
            pump();
            app.reset();
            pump();
        }
        recorder = nullptr;
        UnregisterClassW(L"RefarterDesktopShell", GetModuleHandleW(nullptr)); // The next App registers it again.
    }

    // Quits and starts again over the same files.
    bool reopen() {
        close();
        return open();
    }

    ~AppTestAccess() {
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        close();
        for (const HWND window : windows) DestroyWindow(window);
        RegDeleteTreeW(HKEY_CURRENT_USER, startupTestKey.c_str());
        localAppDataOverride().clear();
        std::error_code error;
        std::filesystem::remove_all(folder, error);
    }

    void writeIni(const Setup& setup) const {
        const auto shortcut = [](UINT key) { return hotkeyText({MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN, key}); };
        const std::wstring ini =
            L"[recording]\r\nsource_monitor=" + setup.sourceMonitor + L"\r\nreplay_seconds=" + std::to_wstring(setup.seconds) + L"\r\nauto_capture=off\r\nauto_capture_prompt=off\r\n"
            L"[video]\r\nresolution=" + resolutionNames[static_cast<int>(setup.resolution)] +
            L"\r\nfps=60\r\nquality=" + qualityNames[static_cast<int>(setup.quality)] +
            L"\r\nbitrate_mbps=" + std::to_wstring(setup.bitrate) + L"\r\ncodec=h264\r\n"
            L"[audio]\r\ndesktop_device=off\r\nmicrophone_device=off\r\nchime_volume=0\r\n"
            L"[storage]\r\nclip_folder=" + (folder / L"clips").wstring() + L"\r\n"
            L"[window]\r\nclose_to_tray=off\r\nwelcome_pending=" + std::wstring(setup.welcomePending ? L"on" : L"off") + L"\r\nstart_replay=" + (setup.signInReplay ? L"on" : L"off") + L"\r\n"
            L"[updates]\r\ncheck=off\r\n"
            L"[shortcuts]\r\nsave_replay=" + shortcut(VK_F23) + L"\r\nscreenshot=" + shortcut(VK_F24) + L"\r\n";
        std::ofstream output(folder / L"refarter.ini", std::ios::binary);
        output.write("\xff\xfe", 2);
        output.write(reinterpret_cast<const char*>(ini.data()), static_cast<std::streamsize>(ini.size() * sizeof(wchar_t)));
    }

    // A hidden popup stands in for the game's window: the App measures its client area as it would a game's.
    RecordingTarget game(const std::wstring& path, const std::wstring& label, int width = 1280, int height = 720) {
        const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", label.c_str(), WS_POPUP,
            0, 0, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        windows.push_back(window);
        RecordingTarget target;
        target.source.window = window;
        target.pid = GetCurrentProcessId();
        target.name = std::filesystem::path(path).filename().wstring();
        target.title = target.displayName = label;
        target.path = path;
        return target;
    }

    // What chooseSource does with a picked game window (its menu lists only real, visible windows), then command()'s tail.
    void choose(RecordingTarget target) {
        app->target_ = std::move(target);
        app->sourceMonitor_.clear();
        app->setStatus(L"source selected · enable replay when ready", true);
        app->followSource();
        app->saveSettings();
        app->refreshControls();
        app->refreshPages();
        pump();
    }

    // Typing: the edit raises EN_CHANGE as for a key press. Leaving: the field's own WM_KILLFOCUS.
    void type(int id, const wchar_t* text) { SetWindowTextW(app->control(id), text); }
    void leave(int id) { SendMessageW(app->control(id), WM_KILLFOCUS, 0, 0); }
    // A click as the button sends it; false (and nothing sent) while the button is disabled.
    bool click(int id) {
        const HWND button = app->control(id);
        if (!IsWindowEnabled(button)) return false;
        SendMessageW(app->window_, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), reinterpret_cast<LPARAM>(button));
        pump();
        return true;
    }
    // Opens a menu by its button and picks the item starting with `prefix`; false if the button or item was missing.
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
    bool chooseFor(const std::wstring& prefix) { return pick(SettingsFor, prefix); }

    std::wstring text(int id) const {
        wchar_t value[64]{};
        GetWindowTextW(app->control(id), value, static_cast<int>(std::size(value)));
        return value;
    }
    // Shown as placed (the main window itself stays hidden).
    bool shown(int id) const { return (GetWindowLongPtrW(app->control(id), GWL_STYLE) & WS_VISIBLE) != 0; }
    bool enabled(int id) const { return IsWindowEnabled(app->control(id)) != FALSE; }
    // The text colour the App gives a number field (WM_CTLCOLOREDIT), as it would when the field paints.
    COLORREF colourOf(int id) const {
        const HDC dc = CreateCompatibleDC(nullptr);
        SendMessageW(app->window_, WM_CTLCOLOREDIT, reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(app->control(id)));
        const COLORREF colour = GetTextColor(dc);
        DeleteDC(dc);
        return colour;
    }
    // What game_settings.dat holds for a game now, read back from disk.
    std::optional<GameVideo> stored(const std::wstring& path) const {
        const auto games = readGameSettings(gameSettingsPath());
        const auto* entry = games ? games->find(path) : nullptr;
        return entry ? std::optional(entry->video) : std::nullopt;
    }
    const GameVideo* own(const std::wstring& path) const {
        const auto* entry = app->gameSettings_.find(path);
        return entry ? &entry->video : nullptr;
    }
    const FakeRecorder::Start* lastStart() const { return recorder->starts.empty() ? nullptr : &recorder->starts.back(); }
    // The recorder answers a start, as from its worker.
    void started() {
        RecordingEvent event{RecordingEvent::Kind::Started};
        event.state = capture::RecorderState::Buffering;
        event.message = L"Buffering · 1280 × 720 · 60 fps · h.264 · cbr 50.0 mbps";
        recorder->notify(std::move(event));
        pump();
    }
    // Holds game_settings.dat open without share-delete or write, so its atomic replace fails as with a real lock;
    // true once a replace has been seen to fail.
    bool lockGameFile() {
        lock = CreateFileW(gameSettingsPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        return lock != INVALID_HANDLE_VALUE && !writeGameSettings(gameSettingsPath(), app->gameSettings_);
    }
    std::wstring footer() const { return app->sessionLine().source; }

    struct RelayTrace { int count = 0; HWND target = nullptr; };
    static LRESULT CALLBACK traceRelay(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data) {
        if (message == TTM_RELAYEVENT) {
            auto& trace = *reinterpret_cast<RelayTrace*>(data);
            ++trace.count;
            trace.target = reinterpret_cast<const MSG*>(lParam)->hwnd;
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }
    static int runAll();
};

int AppTestAccess::runAll() {
    using Quality = capture::Quality; // Not Resolution: ui::Resolution is the control.
    const auto run = [](const char* row, const Setup& setup, auto body) {
        const int before = failures;
        {
            AppTestAccess h(setup);
            if (h.ok) body(h);
            else check(false, std::string(row) + ": the app could not be created");
        }
        std::cout << (failures == before ? "pass  " : "FAIL  ") << row << '\n';
    };
    const auto x60 = gameX(GameVideo{.replaySeconds = 60});

    run("T1 typed length stored when the field is left", Setup{.games = list({x60})}, [](AppTestAccess& h) {
        check(h.chooseFor(L"x game") && h.text(CustomSeconds) == L"60", "T1 precondition: the page is on X, showing 60");
        h.type(CustomSeconds, L"90");
        h.leave(CustomSeconds);
        const auto x = h.stored(xPath);
        check(x && x->replaySeconds == 90, "T1 the file has X replay_seconds=90");
        check(h.text(CustomSeconds) == L"90", "T1 the field shows 90");
        check(h.colourOf(CustomSeconds) == accent, "T1 the field is in the accent colour");
        check(h.shown(ReplayReset), "T1 the length's reset arrow is shown");
    });

    // "6" on the way from 60 to nothing must not be stored.
    run("T2 cleared field stores nothing", Setup{.games = list({x60})}, [](AppTestAccess& h) {
        check(h.chooseFor(L"x game"), "T2 precondition: the page is on X");
        h.type(CustomSeconds, L"6");
        h.type(CustomSeconds, L"");
        h.leave(CustomSeconds);
        const auto x = h.stored(xPath);
        check(x && x->replaySeconds == 60 && h.own(xPath) && h.own(xPath)->replaySeconds == 60, "T2 X's stored length stays 60");
        check(!h.app->state_.replayInputValid, "T2 the field is invalid");
        check(!h.app->typedSeconds_, "T2 nothing typed is waiting");
    });

    run("T3 start without leaving the field", Setup{.games = list({x60})}, [](AppTestAccess& h) {
        h.choose(h.game(xPath, L"X Game"));
        check(samePath(h.app->settingsFor_, xPath) && h.text(CustomSeconds) == L"60", "T3 precondition: the page follows X onto its 60 s");
        h.type(CustomSeconds, L"90");
        check(h.click(StartReplay), "T3 enable replay can be clicked");
        const auto* start = h.lastStart();
        check(start && samePath(start->target.path, xPath) && start->video.replaySeconds == 90, "T3 the recorder gets 90 s");
        const auto x = h.stored(xPath);
        check(x && x->replaySeconds == 90, "T3 the file has 90");
        h.started();
        check(h.footer().find(L"own settings") != std::wstring::npos, "T3 the footer says own settings");
    });

    // matonico 1: recording the old length while the page shows the new one would be a silent swap.
    run("T4 locked file blocks the source's start", Setup{.games = list({x60})}, [](AppTestAccess& h) {
        h.choose(h.game(xPath, L"X Game"));
        check(h.lockGameFile(), "T4 precondition: the locked file refuses a replace");
        h.type(CustomSeconds, L"90");
        h.click(StartReplay);
        check(h.recorder->starts.empty(), "T4 nothing starts");
        check(h.app->status_ == L"game settings could not be saved", "T4 the status says game settings could not be saved");
        check(h.app->typedSeconds_ == 90 && h.text(CustomSeconds) == L"90", "T4 90 is still typed");
    });

    run("T5 locked file keeps the page on X", Setup{.games = list({x60, gameY(GameVideo{.replaySeconds = 120})})}, [](AppTestAccess& h) {
        check(h.chooseFor(L"x game"), "T5 precondition: the page is on X");
        check(h.lockGameFile(), "T5 precondition: the locked file refuses a replace");
        h.type(CustomSeconds, L"90");
        check(h.chooseFor(L"y game"), "T5 the menu lists Y");
        check(samePath(h.app->settingsFor_, xPath), "T5 the page stays on X");
        check(h.app->typedSeconds_ == 90 && h.text(CustomSeconds) == L"90", "T5 90 is still typed");
        check(h.app->status_ == L"game settings could not be saved", "T5 the status says not saved");
    });

    run("T6 locked file does not block another game's start",
        Setup{.games = list({x60, gameY(GameVideo{.resolution = capture::Resolution::P720, .replaySeconds = 120})})}, [](AppTestAccess& h) {
        h.choose(h.game(yPath, L"Y Game"));
        check(h.chooseFor(L"x game"), "T6 precondition: the page is on X, the source is Y");
        check(h.lockGameFile(), "T6 precondition: the locked file refuses a replace");
        h.type(CustomSeconds, L"90");
        check(h.click(StartReplay), "T6 enable replay can be clicked");
        const auto* start = h.lastStart();
        check(start && samePath(start->target.path, yPath) && start->video.replaySeconds == 120 &&
              start->video.resolution == capture::Resolution::P720, "T6 Y starts with Y's values");
    });

    // A hidden bitrate field's stale invalid flag must not block or bend a start.
    run("T7 hidden stale bitrate flag", Setup{.quality = Quality::Custom, .bitrate = 50, .games = list({gameX(GameVideo{.quality = Quality::Custom})})},
        [](AppTestAccess& h) {
        h.type(Bitrate, L"0");
        check(!h.app->bitrateValid_, "T7 precondition: the bitrate field is invalid");
        check(h.pick(QualityPreset, L"balanced"), "T7 all games' quality set to balanced");
        check(h.text(Bitrate) == L"50" && h.app->bitrateValid_, "T7 the bitrate field is back to 50 and valid");
        // Auto-capture's start: the page is still on all games when the replay starts (followSource comes after).
        check(h.app->startAutoReplay(h.game(xPath, L"X Game")), "T7 X starts");
        const auto* start = h.lastStart();
        check(start && start->video.quality == Quality::Custom && start->video.bitrateMbps == 50,
              "T7 X starts at custom quality with all games' bitrate");
    });

    // A game's bitrate belongs to its custom quality.
    run("T8 preset quality clears the game's bitrate",
        Setup{.games = list({gameX(GameVideo{.quality = Quality::Custom, .bitrateMbps = 40})})}, [](AppTestAccess& h) {
        h.choose(h.game(xPath, L"X Game"));
        check(samePath(h.app->settingsFor_, xPath), "T8 precondition: the page is on X");
        check(h.pick(QualityPreset, L"balanced"), "T8 balanced chosen for X");
        check(!h.own(xPath) && !h.stored(xPath), "T8 X's own quality and bitrate are cleared (entry removed)");
        check(h.click(StartReplay), "T8 enable replay can be clicked");
        h.started();
        check(!h.app->sessionOwn_ && h.footer().find(L"own settings") == std::wstring::npos, "T8 the start of X has no own settings");
    });

    run("T9 bitrate only at custom", Setup{.quality = Quality::Custom, .bitrate = 50, .games = list({gameX(GameVideo{.bitrateMbps = 40})})},
        [](AppTestAccess& h) {
        check(h.pick(QualityPreset, L"balanced"), "T9 all games switch to balanced");
        std::vector<std::pair<UINT, std::wstring>> items;
        h.menu = [&](HMENU shown) { menuItems(shown, items); return 0u; };
        h.click(SettingsFor);
        h.menu = nullptr;
        // Fresh review 1: X's bitrate belonged to the custom quality it inherited, so it goes with it (and X with it).
        const bool bitrate = std::any_of(items.begin(), items.end(), [](const auto& item) { return item.second.find(L"40 mbps") != std::wstring::npos; });
        check(!bitrate && !h.own(xPath) && !h.stored(xPath), "T9 all games leaving custom drops X's unused bitrate");
        h.choose(h.game(xPath, L"X Game"));
        check(h.click(StartReplay), "T9 enable replay can be clicked");
        h.started();
        check(!h.app->sessionOwn_ && h.footer().find(L"own settings") == std::wstring::npos, "T9 sessionOwn_ is false");
    });

    run("T10 all games' resolution clears the game's", Setup{.resolution = capture::Resolution::P1440, .games = list({gameX(GameVideo{.resolution = capture::Resolution::P1080})})},
        [](AppTestAccess& h) {
        h.choose(h.game(xPath, L"X Game"));
        check(h.shown(ResolutionReset), "T10 precondition: X's resolution has its reset arrow");
        check(h.pick(Resolution, L"1440p"), "T10 1440p chosen for X");
        check(!(h.own(xPath) && h.own(xPath)->resolution) && !(h.stored(xPath) && h.stored(xPath)->resolution), "T10 X's own resolution is cleared");
        check(!h.app->scopeOwns(ResolutionReset) && !h.shown(ResolutionReset), "T10 the row is not accent (no own value, arrow hidden)");
    });

    run("T11 typed all-games length clears the game's", Setup{.games = list({x60})}, [](AppTestAccess& h) {
        check(h.chooseFor(L"x game"), "T11 precondition: the page is on X");
        h.type(CustomSeconds, L"180");
        h.leave(CustomSeconds);
        check(!(h.own(xPath) && h.own(xPath)->replaySeconds) && !(h.stored(xPath) && h.stored(xPath)->replaySeconds), "T11 X's own length is cleared");
    });

    // A length preset must not rewrite the bitrate field.
    run("T12 length preset leaves the bitrate field", Setup{.quality = Quality::Custom, .bitrate = 50}, [](AppTestAccess& h) {
        h.type(Bitrate, L"0");
        check(h.click(OneMinute), "T12 the 1 min preset can be clicked");
        check(h.app->state_.replaySeconds == 60 && h.text(CustomSeconds) == L"60", "T12 the length is 60");
        check(h.text(Bitrate) == L"0" && !h.app->bitrateValid_, "T12 the bitrate field still shows 0 and is invalid");
    });

    // The restart after sleep replaces the status; the codec note must survive it.
    run("T13 codec note after the sleep restart", Setup{.games = list({gameX(GameVideo{.codec = capture::Codec::HEVC})})}, [](AppTestAccess& h) {
        h.app->codecs_ = hevcRefused();
        h.choose(h.game(xPath, L"X Game"));
        check(h.click(StartReplay), "T13 enable replay can be clicked");
        h.started();
        // Windows suspends (the stand-in answers the pause), then resumes; the settle delay's one-shot timer is sent
        // now rather than waited for.
        SendMessageW(h.app->window_, WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
        check(h.recorder->pauses == 1 && h.app->recording_ == capture::RecorderState::Off, "T13 precondition: paused for sleep");
        SendMessageW(h.app->window_, WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
        SendMessageW(h.app->window_, WM_TIMER, resumeTimer, 0);
        check(h.recorder->starts.size() == 2 && h.lastStart()->video.codec == capture::Codec::H264, "T13 the restart records h.264");
        h.started();
        check(h.app->status_ == L"restarted after sleep · hevc failed this pc's check · recording h.264", "T13 the status keeps the codec note");
    });

    run("T14 codec note after a manual start", Setup{.games = list({gameX(GameVideo{.codec = capture::Codec::HEVC})})}, [](AppTestAccess& h) {
        h.app->codecs_ = hevcRefused();
        h.choose(h.game(xPath, L"X Game"));
        check(h.click(StartReplay), "T14 enable replay can be clicked");
        check(h.lastStart() && h.lastStart()->video.codec == capture::Codec::H264, "T14 the recorder gets h.264");
        h.started();
        check(h.app->status_.ends_with(L" · hevc failed this pc's check · recording h.264"), "T14 the status ends with the codec note");
    });

    // The page's estimates describe the game being edited, not whatever the source is now.
    run("T15 estimates use the chosen game's size", Setup{}, [](AppTestAccess& h) {
        const auto screen = primaryScreen();
        const capture::Size xSize = screen == capture::Size{2560, 1440} ? capture::Size{3440, 1440} : capture::Size{2560, 1440};
        h.choose(h.game(xPath, L"X Game", xSize.width, xSize.height)); // X is measured while it is the source.
        h.app->setScope(xPath, L"X Game");
        const auto estimateAsSource = h.app->estimateText();
        const auto helpAsSource = h.app->codecHelpMarkdown();
        h.choose(h.game(aPath, L"A Game", 1280, 720));
        check(h.app->sourceSize() == capture::Size{1280, 720}, "T15 precondition: source A measures 1280×720");
        const auto estimateOfA = h.app->estimateText(); // The page followed A onto all games.
        h.app->setScope(xPath, L"X Game");
        const auto basis = h.app->scopeSize();
        check(basis.size == xSize && !basis.guessed, "T15 the page on X uses X's last measured size");
        check(h.app->estimateText() == estimateAsSource && estimateAsSource != estimateOfA, "T15 the estimate uses X's size");
        const auto help = h.app->codecHelpMarkdown();
        const auto sizeText = std::to_wstring(xSize.width) + L"\u00d7" + std::to_wstring(xSize.height);
        check(help == helpAsSource && help.find(sizeText) != std::wstring::npos && help.find(L"your screen") == std::wstring::npos,
              "T15 the codec help uses X's size");
        h.app->setScope(zPath, L"Z Game");
        const auto guess = h.app->scopeSize();
        check(guess.guessed && guess.size == screen, "T15 a never-measured game uses the main screen");
        check(h.app->codecHelpMarkdown().find(L"(your screen, until z game is chosen as a source)") != std::wstring::npos,
              "T15 the help says it is the screen's size");
    });

    run("T16 scope follows the source", Setup{.games = list({x60, gameY(GameVideo{.replaySeconds = 120})})}, [](AppTestAccess& h) {
        const auto x = h.game(xPath, L"X Game");
        h.choose(x);
        check(samePath(h.app->settingsFor_, xPath), "T16 a source game with its own values: the page follows it");
        check(h.pick(SelectProcess, L"monitor 1"), "T16 monitor 1 chosen as the source");
        check(h.app->settingsFor_.empty(), "T16 a monitor source: the page is on all games");
        h.choose(x);
        check(h.chooseFor(L"y game") && samePath(h.app->settingsFor_, yPath), "T16 precondition: the user chose Y");
        h.choose(x); // The same game again.
        check(samePath(h.app->settingsFor_, yPath), "T16 the same game again keeps the user's choice");
    });

    run("T17 monitor source records all games' values",
        Setup{.games = list({gameX(GameVideo{.resolution = capture::Resolution::P720, .replaySeconds = 60})})}, [](AppTestAccess& h) {
        check(h.pick(SelectProcess, L"monitor 1") && h.chooseFor(L"x game"), "T17 precondition: monitor source, page on X");
        check(h.click(StartReplay), "T17 enable replay can be clicked");
        const auto* start = h.lastStart();
        check(start && start->video.replaySeconds == 180 && start->video.resolution == capture::Resolution::Native, "T17 the recorder gets all games' values");
        h.started();
        check(!h.app->sessionOwn_ && h.footer().find(L"own settings") == std::wstring::npos, "T17 no own settings");
    });

    run("T18 the source's values, not the page's", Setup{.games = list({x60, gameY(GameVideo{.replaySeconds = 120})})}, [](AppTestAccess& h) {
        h.choose(h.game(xPath, L"X Game"));
        check(h.chooseFor(L"y game"), "T18 precondition: the page is on Y");
        check(h.click(StartReplay), "T18 enable replay can be clicked");
        const auto* start = h.lastStart();
        check(start && samePath(start->target.path, xPath) && start->video.replaySeconds == 60, "T18 the recorder gets X's values");
    });

    run("T19 automatic start", Setup{.games = list({x60})}, [](AppTestAccess& h) {
        check(h.app->startAutoReplay(h.game(xPath, L"X Game")), "T19 the automatic start goes ahead");
        const auto* start = h.lastStart();
        check(start && start->video.replaySeconds == 60, "T19 the recorder gets X's values");
        // Fresh review 3: an automatic start leaves the page alone (the user may be typing in it).
        check(h.app->settingsFor_.empty(), "T19 the page stays where it was");
    });

    run("T20 use defaults", Setup{.games = list({gameX(GameVideo{.quality = Quality::Custom, .bitrateMbps = 40, .replaySeconds = 60})})},
        [](AppTestAccess& h) {
        check(h.chooseFor(L"x game") && h.text(CustomSeconds) == L"60" && h.text(Bitrate) == L"40", "T20 precondition: the page shows X's values");
        check(h.click(UseDefaults), "T20 use defaults can be clicked");
        check(!h.own(xPath) && !h.stored(xPath), "T20 X's entry is removed");
        check(h.text(CustomSeconds) == L"180" && h.text(Bitrate) == L"50", "T20 the fields show all games' values");
        check(samePath(h.app->settingsFor_, xPath), "T20 the page stays on X");
    });

    run("T21 one reset arrow clears one value",
        Setup{.quality = Quality::Custom, .bitrate = 50, .games = list({gameX(GameVideo{.resolution = capture::Resolution::P720, .bitrateMbps = 40})})},
        [](AppTestAccess& h) {
        check(h.chooseFor(L"x game"), "T21 precondition: the page is on X");
        h.type(Bitrate, L"0");
        check(h.click(ResolutionReset), "T21 the resolution's reset arrow can be clicked");
        const auto* x = h.own(xPath);
        check(x && !x->resolution && x->bitrateMbps == 40, "T21 only the resolution is cleared");
        check(h.text(Bitrate) == L"0" && !h.app->bitrateValid_, "T21 the bitrate text and its validity are untouched");
    });

    run("T22 foreign file", Setup{.gameFileText = L"refarter game-settings 99\ngame \"C:\\\\Games\\\\X\\\\x.exe\" \"X Game\" replay_seconds=60\n"},
        [](AppTestAccess& h) {
        const auto before = fileBytes(gameSettingsPath());
        check(!before.empty() && !h.app->gameSettingsReadable_, "T22 per-game settings are off");
        check(!h.enabled(SettingsFor), "T22 the drop-down is disabled");
        h.choose(h.game(xPath, L"X Game"));
        // A menu that was already open when it was disabled: its choice changes nothing.
        h.menu = [](HMENU shown) { return itemStarting(shown, L"x game"); };
        h.app->chooseSettingsFor();
        h.menu = nullptr;
        check(h.app->settingsFor_.empty(), "T22 the page stays on all games");
        h.type(CustomSeconds, L"90");
        h.leave(CustomSeconds);
        h.click(OneMinute);
        h.pick(QualityPreset, L"high");
        h.pick(Resolution, L"720p");
        check(fileBytes(gameSettingsPath()) == before, "T22 the file's bytes are unchanged");
    });

    run("T23 quit stores what is typed", Setup{.games = list({x60})}, [](AppTestAccess& h) {
        check(h.chooseFor(L"x game"), "T23 precondition: the page is on X");
        h.type(CustomSeconds, L"90");
        SendMessageW(h.app->window_, WM_CLOSE, 0, 0); // Quit (close to tray is off) with the field not left.
        pump();
        check(!IsWindow(h.app->window_), "T23 the window closes once the recorder has closed");
        const auto x = h.stored(xPath);
        check(x && x->replaySeconds == 90, "T23 the file has 90");
    });

    run("T24 the page locks while replay runs", Setup{.games = list({x60, gameY(GameVideo{.replaySeconds = 120})})}, [](AppTestAccess& h) {
        // The drop-down's menu is open when auto-capture starts X; Y is chosen in it afterwards.
        bool listed = false;
        const auto before = h.app->settingsFor_;
        h.menu = [&](HMENU shown) {
            const UINT y = itemStarting(shown, L"y game");
            listed = y != 0;
            h.app->startAutoReplay(h.game(xPath, L"X Game"));
            return y;
        };
        h.click(SettingsFor);
        h.menu = nullptr;
        check(listed && h.recorder->starts.size() == 1, "T24 precondition: X started while the menu was open");
        check(h.app->settingsFor_ == before, "T24 the choice made after the start is ignored");
        h.started();
        check(!h.enabled(SettingsFor) && !h.enabled(UseDefaults), "T24 the drop-down and use defaults are disabled");
        h.menu = [](HMENU shown) { return itemStarting(shown, L"high"); }; // A quality menu still open over the start.
        h.app->chooseQuality();
        h.menu = nullptr;
        check(h.own(xPath) && !h.own(xPath)->quality, "T24 a quality chosen after the start is ignored");
    });

    run("T25 invalid all-games length, source owns its length", Setup{.games = list({gameX(GameVideo{.replaySeconds = 120})})}, [](AppTestAccess& h) {
        h.choose(h.game(xPath, L"X Game"));
        check(h.chooseFor(L"all games"), "T25 precondition: the page is on all games");
        h.type(CustomSeconds, L"2");
        check(!h.app->state_.replayInputValid, "T25 precondition: the length field is invalid");
        check(h.click(StartReplay), "T25 enable replay can be clicked");
        const auto* start = h.lastStart();
        check(start && start->video.replaySeconds == 120, "T25 X starts with its own length");
    });

    // Rows T26–T31 come from the fresh review of PR 166 (numbers in the comments).
    run("T26 the quality reset arrow drops the game's bitrate",
        Setup{.games = list({gameX(GameVideo{.quality = Quality::Custom, .bitrateMbps = 40})})}, [](AppTestAccess& h) {
        // Review 1: all games balanced, X custom at 40; X's quality arrow: X follows balanced and its bitrate goes too.
        check(h.chooseFor(L"x game") && h.shown(QualityReset), "T26 precondition: the page is on X, its quality arrow shown");
        check(h.click(QualityReset), "T26 the quality arrow can be clicked");
        check(!h.own(xPath) && !h.stored(xPath) && !h.shown(UseDefaults), "T26 X is left with nothing of its own");
    });

    run("T27 the page follows a source again after a failed save", Setup{.games = list({x60, gameY(GameVideo{.replaySeconds = 120})})},
        [](AppTestAccess& h) {
        // Review 2: a failed save keeps the page on X; once saving works, the next change of source still moves it.
        h.choose(h.game(xPath, L"X Game"));
        check(h.lockGameFile(), "T27 precondition: the locked file refuses a replace");
        h.type(CustomSeconds, L"90");
        const auto y = h.game(yPath, L"Y Game");
        h.choose(y);
        check(samePath(h.app->settingsFor_, xPath) && h.app->typedSeconds_ == 90, "T27 the page stays on X with 90 typed");
        CloseHandle(h.lock);
        h.lock = INVALID_HANDLE_VALUE;
        h.choose(y);
        check(samePath(h.app->settingsFor_, yPath), "T27 choosing Y again moves the page once the save works");
        check(h.stored(xPath) && h.stored(xPath)->replaySeconds == 90, "T27 X's 90 is stored on the way");
    });

    run("T28 an automatic start while typing stores nothing", Setup{.games = list({x60, gameY(GameVideo{.replaySeconds = 120})})},
        [](AppTestAccess& h) {
        // Review 3: halfway through typing 90 for X, auto-capture starts Y.
        check(h.chooseFor(L"x game"), "T28 precondition: the page is on X");
        h.type(CustomSeconds, L"9");
        check(h.app->startAutoReplay(h.game(yPath, L"Y Game")), "T28 the automatic start goes ahead");
        check(h.lastStart() && h.lastStart()->video.replaySeconds == 120, "T28 Y records its own length");
        check(h.stored(xPath) && h.stored(xPath)->replaySeconds == 60, "T28 nothing half-typed is stored for X");
        check(h.text(CustomSeconds) == L"9" && h.app->typedSeconds_ == 9 && samePath(h.app->settingsFor_, xPath),
              "T28 the field and the page are left as they were");
    });

    run("T29 a refused game codec shows as h.264 on the page", Setup{.games = list({gameX(GameVideo{.codec = capture::Codec::HEVC})})},
        [](AppTestAccess& h) {
        // Review 4: the check refuses hevc; the page on X says what X records with.
        h.app->codecs_ = hevcRefused();
        check(h.chooseFor(L"x game"), "T29 precondition: the page is on X");
        check(h.app->scopeRecordVideo().codec == capture::Codec::H264, "T29 estimates and help use h.264");
        std::wstring note;
        for (const auto& section : h.app->pages_[static_cast<size_t>(Page::Recording)])
            for (const auto& row : section.rows) if (std::wstring_view(row.name) == L"codec") note = row.note;
        check(note == L"hevc can't record on this pc · records h.264", "T29 the codec row says hevc records as h.264");
    });

    run("T30 a refused codec alone is not own settings", Setup{.games = list({gameX(GameVideo{.codec = capture::Codec::HEVC})})},
        [](AppTestAccess& h) {
        // Review 5: X's only own value is hevc, which the check refuses: X records all games' values and h.264.
        h.app->codecs_ = hevcRefused();
        h.choose(h.game(xPath, L"X Game"));
        check(h.click(StartReplay), "T30 enable replay can be clicked");
        h.started();
        check(!h.app->sessionOwn_ && h.footer().find(L"own settings") == std::wstring::npos, "T30 no own settings");
    });

    run("T31 a preset that cannot be saved keeps what was typed", Setup{.games = list({x60})}, [](AppTestAccess& h) {
        // Review 6: 90 typed for X, the file locked, the 3 min preset: not saved, so nothing is treated as applied.
        check(h.chooseFor(L"x game"), "T31 precondition: the page is on X");
        h.type(CustomSeconds, L"90");
        check(h.lockGameFile(), "T31 precondition: the locked file refuses a replace");
        check(h.click(ThreeMinutes), "T31 the 3 min preset can be clicked");
        check(h.app->typedSeconds_ == 90 && h.text(CustomSeconds) == L"90", "T31 90 is still typed");
        check(h.app->status_ == L"game settings could not be saved", "T31 the status says it was not saved");
    });

    // Rows T32–T37 come from the second fresh review of PR 166 (numbers in the comments).
    run("T32 another game's failed save is still reported at a start", Setup{.games = list({x60, gameY(GameVideo{.replaySeconds = 120})})},
        [](AppTestAccess& h) {
        // Review 1: source X, page on Y, 45 typed for Y, file locked; start: X starts, the status keeps the failure.
        h.choose(h.game(xPath, L"X Game"));
        check(h.chooseFor(L"y game"), "T32 precondition: the page is on Y");
        h.type(CustomSeconds, L"45");
        check(h.lockGameFile(), "T32 precondition: the locked file refuses a replace");
        check(h.click(StartReplay), "T32 enable replay can be clicked");
        check(h.lastStart() && h.lastStart()->video.replaySeconds == 60, "T32 X starts with its own length");
        check(h.app->status_.find(L"game settings could not be saved") != std::wstring::npos, "T32 the starting status says so");
        h.started();
        check(h.app->status_.ends_with(L" · game settings could not be saved"), "T32 the started status still says so");
        check(h.app->typedSeconds_ == 45, "T32 45 is still typed for Y");
    });

    run("T33 a game codec refused by the check shows once the check ends", Setup{.games = list({gameX(GameVideo{.codec = capture::Codec::HEVC})})},
        [](AppTestAccess& h) {
        // Review 2: the page is on X before the check has run; its result refreshes the codec row.
        check(h.chooseFor(L"x game"), "T33 precondition: the page is on X");
        RecordingEvent event{RecordingEvent::Kind::Codecs};
        event.codecs = hevcRefused();
        h.recorder->notify(std::move(event));
        pump();
        std::wstring note;
        for (const auto& section : h.app->pages_[static_cast<size_t>(Page::Recording)])
            for (const auto& row : section.rows) if (std::wstring_view(row.name) == L"codec") note = row.note;
        check(note == L"hevc can't record on this pc · records h.264", "T33 the codec row says hevc records as h.264");
    });

    run("T34 quitting with a failed save says so", Setup{.games = list({x60})}, [](AppTestAccess& h) {
        // Review 3: 90 typed for X, file locked, quit: Refarter still closes, says the value was not saved, file intact.
        check(h.chooseFor(L"x game"), "T34 precondition: the page is on X");
        h.type(CustomSeconds, L"90");
        check(h.lockGameFile(), "T34 precondition: the locked file refuses a replace");
        h.app->quit();
        pump();
        check(h.app->closing_, "T34 Refarter still closes");
        check(h.app->status_.ends_with(L" · game settings could not be saved"), "T34 the closing status says the value was not saved");
        check(h.stored(xPath) && h.stored(xPath)->replaySeconds == 60, "T34 the file keeps its old value");
    });

    run("T35 a tab in a game's name never splits its menu row", Setup{.games = list({{xPath, L"X Game\tDeluxe", GameVideo{.replaySeconds = 60}}})},
        [](AppTestAccess& h) {
        // Review 4: a tab would make the menu draw "deluxe" as grey detail text. (Not a platform tag such as "dx12",
        // which the short name leaves out.)
        std::vector<std::pair<UINT, std::wstring>> items;
        h.menu = [&](HMENU shown) { menuItems(shown, items); return 0u; };
        h.click(SettingsFor);
        h.menu = nullptr;
        const bool joined = std::any_of(items.begin(), items.end(), [](const auto& item) { return lower(item.second).starts_with(L"x game deluxe"); });
        const bool tab = std::any_of(items.begin(), items.end(), [](const auto& item) { return item.second.find(L'\t') != std::wstring::npos && lower(item.second).starts_with(L"x game"); });
        check(joined && !tab, "T35 the row reads 'x game deluxe' with no tab");
        h.app->setScope(zPath, L"Z\tGame");
        check(h.app->settingsForLabel_ == L"Z Game", "T35 a label set from a window title loses its tab too");
    });

    run("T35b short game names are display only and follow the switch",
        Setup{.games = list({{xPath, L"Stardew Valley 1.6.15 - running SMAPI", GameVideo{.replaySeconds = 60}}})}, [](AppTestAccess& h) {
        const auto rows = [&] {
            std::vector<std::pair<UINT, std::wstring>> items;
            h.menu = [&](HMENU shown) { menuItems(shown, items); return 0u; };
            h.click(SettingsFor);
            h.menu = nullptr;
            return items;
        };
        const auto starts = [](const auto& items, const wchar_t* text) {
            return std::any_of(items.begin(), items.end(), [&](const auto& item) { return item.second.starts_with(text); });
        };
        check(starts(rows(), L"stardew valley ·"), "T35b the menu shows the short name");
        h.app->state_.shortGameNames = false;
        check(starts(rows(), L"stardew valley 1.6.15 - running smapi"), "T35b switched off, the menu shows the whole name at once");
        check(h.app->gameSettings_.find(xPath)->label == L"Stardew Valley 1.6.15 - running SMAPI", "T35b the stored name never changes");
        // Review: a value stored for a game chosen under another name (its auto-capture label) keeps the file's name.
        h.app->setScope(xPath, L"stardew valley · stardewmodding.exe");
        check(h.app->storeScope(GameVideo{.replaySeconds = 90}), "T35b precondition: the value is stored");
        check(h.app->gameSettings_.find(xPath)->label == L"Stardew Valley 1.6.15 - running SMAPI", "T35b storing a value keeps the stored name");
    });

    run("T35c renaming a game is display only and resets", Setup{.games = list({gameX({.replaySeconds = 60})})}, [](AppTestAccess& h) {
        const auto caption = [&] {
            wchar_t text[128]{};
            GetWindowTextW(h.app->control(SettingsFor), text, 128);
            return std::wstring(text);
        };
        check(!h.shown(RenameGame), "T35c no pencil while the page edits all games");
        h.app->setScope(xPath, L"X Game");
        h.app->refreshPages();
        pump();
        check(h.shown(RenameGame), "T35c the pencil shows for a chosen game");
        std::wstring offered;
        h.app->answerName_ = [&](const std::wstring& start) { offered = start; return std::optional<std::wstring>(L"  Black Ops 6 "); };
        h.click(RenameGame);
        check(offered == L"x game", "T35c the field starts with the automatic name");
        check(caption() == L"settings for · black ops 6", "T35c the drop-down shows the new name, trimmed and in lower case");
        check(h.app->gameNames_.find(xPath) && readGameNames(gameNamesPath())->games.size() == 1, "T35c the name is saved to game_names.dat");
        check(h.app->gameSettings_.find(xPath)->label == L"X Game", "T35c the stored name never changes");
        h.app->state_.shortGameNames = false;
        check(h.app->displayGameName(xPath, L"X Game") == L"black ops 6", "T35c the short-names switch leaves a custom name alone");
        h.app->answerName_ = [&](const std::wstring& start) { offered = start; return std::optional<std::wstring>(); };
        h.click(RenameGame);
        check(offered == L"Black Ops 6" && h.app->gameNames_.find(xPath), "T35c cancel keeps the name; the field starts with it as typed");
        h.app->answerName_ = [](const std::wstring&) { return std::optional<std::wstring>(L"   "); };
        h.click(RenameGame);
        check(!h.app->gameNames_.find(xPath) && caption() == L"settings for · x game", "T35c an empty name brings back the usual one");
        check(readGameNames(gameNamesPath()) && readGameNames(gameNamesPath())->games.empty(), "T35c the reset is saved");
        // Review: saving the prefilled usual name unchanged must not freeze it as a custom name.
        h.app->answerName_ = [](const std::wstring& start) { return std::optional<std::wstring>(start); };
        h.click(RenameGame);
        check(!h.app->gameNames_.find(xPath), "T35c the usual name saved unchanged stays the usual name");
        // #169 review: other dialogs wait while the name field is open, and what waited is posted once it closes.
        for (const bool save : {true, false}) {
            bool guarded = false;
            h.app->answerName_ = [&](const std::wstring&) {
                guarded = h.app->dialogOpen();
                h.app->queuedErrors_.push_back({L"a save failed", MB_ICONERROR}); // As a failed save under the dialog.
                return save ? std::optional<std::wstring>(L"Guarded") : std::nullopt;
            };
            h.app->renameGame(xPath, L"X Game");
            MSG held{};
            const bool resumed = PeekMessageW(&held, h.app->window_, errorDialogs, errorDialogs, PM_REMOVE) != FALSE;
            check(guarded, save ? "T35c the rename dialog counts as an open dialog" : "T35c a cancelled rename counted too");
            check(!h.app->dialogOpen() && resumed, save ? "T35c after saving, held errors are posted" : "T35c after cancel, held errors are posted");
            h.app->queuedErrors_.clear();
        }
        h.app->answerName_ = nullptr;
    });

    run("T35d an unreadable names file is never written", Setup{.games = list({gameX({.replaySeconds = 60})})}, [](AppTestAccess& h) {
        h.app->gameNamesReadable_ = false; // As after reading a malformed file at start-up.
        h.app->setScope(xPath, L"X Game");
        check(!h.app->renameShown(), "T35d no pencil");
        check(!h.app->setGameName(xPath, L"y") && !std::filesystem::exists(gameNamesPath()), "T35d nothing is written");
    });

    run("T36 a game's name is plain text in the codec help", Setup{}, [](AppTestAccess& h) {
        // Review 6: markdown markers in a name must not turn into formatting or lose text.
        h.app->setScope(zPath, L"**Beta** [EU]");
        const auto help = h.app->codecHelpMarkdown();
        check(help.find(L"(your screen, until beta eu] is chosen as a source)") != std::wstring::npos, "T36 the name shows without markers");
    });

    run("T36b two windows whose titles shorten alike stay apart in the footer", Setup{}, [](AppTestAccess& h) {
        // #167 review: "... server a" and "... server b" both shorten to "minecraft"; the picker's labels must tell them apart.
        const std::wstring javaw = L"C:\\Games\\Minecraft\\javaw.exe";
        std::vector<RecordingTarget> windows{h.game(javaw, L"Minecraft 1.21.4 - Server A"), h.game(javaw, L"Minecraft 1.21.4 - Server B")};
        h.app->labelSources(windows);
        std::vector<std::wstring> shown;
        for (const bool shortNames : {true, false}) {
            h.app->state_.shortGameNames = shortNames;
            for (const auto& window : windows) {
                h.choose(window);
                shown.push_back(h.footer());
            }
        }
        check(shown[0] != shown[1], "T36b with short names the two footers differ");
        check(shown[2] != shown[3], "T36b with whole names the two footers differ");
        check(shown[0].find(L"minecraft · javaw.exe · window 1") != std::wstring::npos, "T36b the short name keeps the window number");
    });

    run("T37 the source window is measured once per refresh", Setup{}, [](AppTestAccess& h) {
        // Review 9: estimates ask for the size many times per key press; a resize is seen once the moment has passed.
        h.choose(h.game(aPath, L"A Game", 1280, 720));
        check(h.app->sourceSize() == capture::Size{1280, 720}, "T37 precondition: A measures 1280×720");
        SetWindowPos(h.app->target_.source.window, nullptr, 0, 0, 1600, 900, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
        check(h.app->sourceSize() == capture::Size{1280, 720}, "T37 within one refresh the measured size is reused");
        Sleep(300);
        check(h.app->sourceSize() == capture::Size{1600, 900}, "T37 a moment later the new size is measured");
    });

    // Rows T38– test renaming games (#168) from outside: docs/gui-checks.md "Renaming games".
    const auto caption = [](AppTestAccess& h) {
        wchar_t text[256]{};
        GetWindowTextW(h.app->control(SettingsFor), text, 256);
        return std::wstring(text);
    };
    const auto menuRows = [](AppTestAccess& h, int button) {
        std::vector<std::pair<UINT, std::wstring>> items;
        h.menu = [&](HMENU shown) { menuItems(shown, items); return 0u; };
        h.click(button);
        h.menu = nullptr;
        std::vector<std::wstring> texts;
        for (const auto& item : items) texts.push_back(item.second);
        return texts;
    };
    const auto anyStarts = [](const std::vector<std::wstring>& rows, const std::wstring& prefix) {
        return std::any_of(rows.begin(), rows.end(), [&](const auto& row) { return row.starts_with(prefix); });
    };
    const auto anyContains = [](const std::vector<std::wstring>& rows, const std::wstring& part) {
        return std::any_of(rows.begin(), rows.end(), [&](const auto& row) { return row.find(part) != std::wstring::npos; });
    };
    const auto rename = [](AppTestAccess& h, std::optional<std::wstring> answer, std::wstring* offered = nullptr) {
        h.app->answerName_ = [=](const std::wstring& start) { if (offered) *offered = start; return answer; };
        const bool clicked = h.click(RenameGame);
        h.app->answerName_ = nullptr;
        return clicked;
    };
    const auto rect = [](AppTestAccess& h, int id) {
        RECT r{};
        GetWindowRect(h.app->control(id), &r);
        return r;
    };

    run("T38 the pencil shows only for a chosen game and moves the drop-down", Setup{.games = list({x60})}, [&](AppTestAccess& h) {
        check(!h.shown(RenameGame), "T38 no pencil while the page edits all games");
        const auto before = rect(h, SettingsFor);
        check(h.chooseFor(L"x game"), "T38 precondition: the page is on X");
        check(h.shown(RenameGame) && h.enabled(RenameGame), "T38 the pencil shows, enabled, for a chosen game");
        const auto pencil = rect(h, RenameGame), moved = rect(h, SettingsFor);
        check(pencil.left >= moved.right, "T38 the pencil is right of the drop-down");
        check(moved.right < before.right && moved.top == pencil.top, "T38 the drop-down moves left to make room, on the pencil's line");
        RECT overlap{};
        const RECT defaults = rect(h, UseDefaults);
        check(!h.shown(UseDefaults) || !IntersectRect(&overlap, &moved, &defaults), "T38 'use defaults' does not overlap the drop-down");
        check(!IntersectRect(&overlap, &moved, &pencil), "T38 the pencil does not overlap the drop-down");
        check(h.chooseFor(L"all games"), "T38 precondition: back on all games");
        check(!h.shown(RenameGame), "T38 the pencil hides again");
        check(rect(h, SettingsFor).right == before.right, "T38 the drop-down returns to its place");
    });

    run("T39 a new name shows everywhere, stored names and save names stay", Setup{.games = list({x60})}, [&](AppTestAccess& h) {
        h.app->autoCapture_.add(xPath, L"X Game");
        check(writeAutoCapture(autoCapturePath(), h.app->autoCapture_), "T39 precondition: X is on the auto-capture list");
        h.choose(h.game(xPath, L"X Game"));
        check(samePath(h.app->settingsFor_, xPath), "T39 precondition: the page follows X");
        const auto saveBefore = h.app->saveName();
        std::wstring offered;
        check(rename(h, L"Black Ops 6", &offered), "T39 the pencil can be clicked");
        check(offered == L"x game", "T39 the field starts with the shown name");
        check(caption(h) == L"settings for · black ops 6", "T39 the drop-down caption shows the name in lower case");
        const auto forRows = menuRows(h, SettingsFor);
        check(anyStarts(forRows, L"black ops 6") && !anyStarts(forRows, L"x game"), "T39 the 'settings for' row shows the new name");
        const auto autoRows = menuRows(h, AutoCaptureSource);
        check(!autoRows.empty(), "T39 precondition: the auto-capture menu opened");
        check(std::count(autoRows.begin(), autoRows.end(), L"black ops 6") == 1, "T39 the slim auto-capture list shows the new name once");
        check(!anyStarts(autoRows, L"auto-capture black ops 6"), "T39 an already-listed game has no duplicate add row");
        check(!anyContains(autoRows, L"x game"), "T39 no auto-capture row keeps the old name");
        check(h.footer().starts_with(L"black ops 6"), "T39 the footer's source shows the new name");
        const auto settings = readGameSettings(gameSettingsPath());
        check(settings && settings->find(xPath) && settings->find(xPath)->label == L"X Game", "T39 game_settings.dat keeps 'X Game'");
        const auto listed = readAutoCapture(autoCapturePath());
        check(listed && listed->find(xPath) && listed->find(xPath)->label == L"X Game", "T39 auto_capture.dat keeps 'X Game'");
        check(h.app->saveName() == saveBefore && saveBefore == friendlySourceName(L"X Game", L"x.exe"), "T39 the clip save name is unchanged");
        // The tray tip names the source only while replay runs.
        check(h.click(StartReplay), "T39 enable replay can be clicked");
        h.started();
        check(h.app->trayTip().find(L"black ops 6") != std::wstring::npos && h.app->trayTip().find(L"x game") == std::wstring::npos,
              "T39 the tray tip shows the new name while replay runs");
        check(h.lastStart() && h.app->saveName() == saveBefore, "T39 the save name while replay runs is still the usual one");
        const auto names = readGameNames(gameNamesPath());
        check(names && names->games.size() == 1 && names->games[0].name == L"Black Ops 6", "T39 game_names.dat keeps the name as typed");
    });

    run("T40 cancelling the dialog changes nothing", Setup{.games = list({x60})}, [&](AppTestAccess& h) {
        check(h.chooseFor(L"x game"), "T40 precondition: the page is on X");
        const auto status = h.app->status_;
        check(rename(h, std::nullopt), "T40 the pencil can be clicked");
        check(caption(h) == L"settings for · x game", "T40 the caption is unchanged");
        check(h.app->gameNames_.games.empty() && !std::filesystem::exists(gameNamesPath()), "T40 no name is kept and no file written");
        check(h.app->status_ == status, "T40 the status is unchanged");
        check(rename(h, L"Named"), "T40 precondition: renamed");
        const auto bytes = fileBytes(gameNamesPath());
        check(rename(h, std::nullopt), "T40 the pencil can be clicked again");
        check(caption(h) == L"settings for · named" && fileBytes(gameNamesPath()) == bytes, "T40 cancelling keeps a custom name and the file");
    });

    run("T41 an empty or spaces-only name brings back the usual name", Setup{.games = list({x60, gameY({.replaySeconds = 90})})},
        [&](AppTestAccess& h) {
        check(h.chooseFor(L"x game") && rename(h, L"Named X"), "T41 precondition: X renamed");
        check(h.chooseFor(L"y game") && rename(h, L"Named Y"), "T41 precondition: Y renamed");
        check(rename(h, L""), "T41 an empty name can be saved");
        check(caption(h) == L"settings for · y game", "T41 empty: Y's usual name returns");
        auto names = readGameNames(gameNamesPath());
        check(names && !names->find(yPath) && names->find(xPath), "T41 empty: the file no longer lists Y and still lists X");
        check(h.chooseFor(L"named x") && rename(h, L" \t\r\n  "), "T41 a whitespace-only name can be saved");
        check(caption(h) == L"settings for · x game", "T41 whitespace: X's usual name returns");
        names = readGameNames(gameNamesPath());
        check(names && names->games.empty(), "T41 whitespace: the file lists no game");
    });

    run("T42 the prefilled usual name saved unchanged is not stored",
        Setup{.games = list({{xPath, L"Stardew Valley 1.6.15 - running SMAPI", GameVideo{.replaySeconds = 60}}})}, [&](AppTestAccess& h) {
        check(h.chooseFor(L"stardew valley"), "T42 precondition: the page is on X");
        std::wstring offered;
        h.app->answerName_ = [&](const std::wstring& start) { offered = start; return std::optional<std::wstring>(start); };
        h.click(RenameGame);
        check(offered == L"stardew valley", "T42 short names on: the field offers the short name");
        check(!h.app->gameNames_.find(xPath) && !std::filesystem::exists(gameNamesPath()), "T42 short names on: nothing stored");
        h.click(ShortGameNames);
        check(!h.app->state_.shortGameNames, "T42 precondition: short names switched off");
        h.click(RenameGame);
        check(offered == L"stardew valley 1.6.15 - running smapi", "T42 short names off: the field offers the whole name");
        check(!h.app->gameNames_.find(xPath) && !std::filesystem::exists(gameNamesPath()), "T42 short names off: nothing stored");
        h.click(ShortGameNames);
        check(caption(h) == L"settings for · stardew valley", "T42 the name still follows the switch");
        h.app->answerName_ = nullptr;
    });

    run("T43 a custom name ignores 'short game names'; others follow it",
        Setup{.games = list({{xPath, L"Stardew Valley 1.6.15 - running SMAPI", GameVideo{.replaySeconds = 60}},
                             {yPath, L"Stardew Valley 1.6.14 - running SMAPI", GameVideo{.replaySeconds = 90}}})}, [&](AppTestAccess& h) {
        check(h.chooseFor(L"stardew valley") && samePath(h.app->settingsFor_, xPath), "T43 precondition: the page is on X");
        check(rename(h, L"My Stardew 1.6 - Modded"), "T43 precondition: X renamed");
        check(caption(h) == L"settings for · my stardew 1.6 - modded", "T43 short names on: the custom name is shown whole");
        auto rows = menuRows(h, SettingsFor);
        check(anyStarts(rows, L"my stardew 1.6 - modded ·") && anyStarts(rows, L"stardew valley ·"), "T43 short names on: menu rows");
        h.click(ShortGameNames);
        check(!h.app->state_.shortGameNames, "T43 precondition: short names switched off");
        h.app->refreshControls();
        check(caption(h) == L"settings for · my stardew 1.6 - modded", "T43 short names off: the custom name is unchanged");
        rows = menuRows(h, SettingsFor);
        check(anyStarts(rows, L"my stardew 1.6 - modded ·") && anyStarts(rows, L"stardew valley 1.6.14 - running smapi ·"),
              "T43 short names off: Y shows its whole name, X its custom name");
        h.click(ShortGameNames);
    });

    run("T44 a name is cut to 80, kept on one line and trimmed", Setup{.games = list({x60})}, [&](AppTestAccess& h) {
        check(h.chooseFor(L"x game"), "T44 precondition: the page is on X");
        check(rename(h, std::wstring(100, L'a')), "T44 a 100-character name can be saved");
        auto custom = h.app->gameNames_.find(xPath);
        check(custom && custom->name == std::wstring(80, L'a'), "T44 it is cut to 80 characters");
        auto names = readGameNames(gameNamesPath());
        check(names && names->find(xPath) && names->find(xPath)->name.size() == 80, "T44 the file keeps 80 characters");
        check(rename(h, L"  Line one\r\nline two\n  "), "T44 a two-line name can be saved");
        custom = h.app->gameNames_.find(xPath);
        check(custom && custom->name.find_first_of(L"\r\n") == std::wstring::npos, "T44 line breaks are gone");
        check(custom && custom->name.starts_with(L"Line one ") && custom->name.ends_with(L" line two"), "T44 line breaks become spaces, ends trimmed");
        names = readGameNames(gameNamesPath());
        check(names && names->find(xPath) && names->find(xPath)->name == custom->name, "T44 the file reads back the same name");
        // The field never offers more than can be kept.
        std::wstring offered;
        check(rename(h, std::wstring(L""), nullptr) && rename(h, std::nullopt, &offered), "T44 precondition: reset, then reopened");
        check(offered == L"x game", "T44 after a reset the field offers the usual name");
    });

    run("T44b a long usual name is offered cut to 80",
        Setup{.games = list({{xPath, std::wstring(90, L'Q'), GameVideo{.replaySeconds = 60}}})}, [&](AppTestAccess& h) {
        h.click(ShortGameNames); // Off: the whole stored name is the usual name.
        h.app->setScope(xPath, std::wstring(90, L'Q'));
        h.app->refreshControls();
        h.app->refreshPages();
        pump();
        std::wstring offered;
        h.app->answerName_ = [&](const std::wstring& start) { offered = start; return std::optional<std::wstring>(start); };
        h.click(RenameGame);
        h.app->answerName_ = nullptr;
        check(offered.size() <= 80, "T44b the field offers at most 80 characters");
        // Saving that offer unchanged: the usual name is longer than 80, so the cut offer differs from it.
        check(!h.app->gameNames_.find(xPath), "T44b saving the offered (cut) usual name unchanged stores no custom name");
    });

    run("T45 switching games shows each one's own name", Setup{.games = list({x60, gameY({.replaySeconds = 90})})}, [&](AppTestAccess& h) {
        check(h.chooseFor(L"x game") && rename(h, L"Alpha"), "T45 precondition: X renamed");
        check(h.chooseFor(L"y game"), "T45 precondition: the page is on Y");
        check(caption(h) == L"settings for · y game", "T45 the caption shows Y, not X's name");
        std::wstring offered;
        check(rename(h, L"Beta", &offered), "T45 Y can be renamed");
        check(offered == L"y game", "T45 the field offers Y's name, not X's");
        check(caption(h) == L"settings for · beta", "T45 the caption shows Y's new name");
        check(h.app->gameNames_.find(xPath) && h.app->gameNames_.find(xPath)->name == L"Alpha", "T45 X keeps its name");
        check(h.chooseFor(L"alpha"), "T45 precondition: back on X by its new name");
        check(caption(h) == L"settings for · alpha", "T45 the caption shows X's name again");
        check(rename(h, std::nullopt, &offered) && offered == L"Alpha", "T45 the field offers X's own custom name");
        check(h.chooseFor(L"all games") && caption(h) == L"settings for · all games", "T45 all games shows no game name");
    });

    run("T46 the pencil is disabled while replay runs or is retained", Setup{.games = list({x60})}, [&](AppTestAccess& h) {
        h.choose(h.game(xPath, L"X Game"));
        check(h.shown(RenameGame) && h.enabled(RenameGame), "T46 precondition: the pencil shows for X");
        check(h.click(StartReplay), "T46 enable replay can be clicked");
        check(!h.enabled(RenameGame), "T46 starting: the pencil is disabled");
        h.started();
        check(!h.enabled(RenameGame) && !h.enabled(CustomSeconds), "T46 running: the pencil is disabled like the page");
        check(!h.click(RenameGame), "T46 running: the pencil does not answer a click");
        check(h.click(StartReplay), "T46 precondition: stop replay can be clicked");
        RecordingEvent stopped{RecordingEvent::Kind::Stopped};
        stopped.state = capture::RecorderState::Stopped;
        h.recorder->notify(std::move(stopped));
        pump();
        check(h.app->recording_ == capture::RecorderState::Stopped, "T46 precondition: the replay is retained");
        check(!h.enabled(RenameGame), "T46 retained: the pencil is disabled");
        RecordingEvent cleared{RecordingEvent::Kind::Stopped};
        cleared.state = capture::RecorderState::Off;
        h.recorder->notify(std::move(cleared));
        pump();
        check(h.enabled(RenameGame), "T46 off again: the pencil is enabled");
    });

    run("T47 a custom name is kept after a restart", Setup{.games = list({x60})}, [&](AppTestAccess& h) {
        check(h.chooseFor(L"x game") && rename(h, L"Black Ops 6"), "T47 precondition: X renamed");
        check(h.reopen(), "T47 Refarter starts again");
        check(h.app->gameNamesReadable_ && h.app->gameNames_.find(xPath), "T47 the new App reads the name");
        check(h.chooseFor(L"black ops 6"), "T47 the 'settings for' menu lists X by its new name");
        check(caption(h) == L"settings for · black ops 6", "T47 the caption shows it");
    });

    for (const auto& [row, text] : {std::pair{"T48 a malformed names file is never written", std::wstring(L"not a names file\n")},
                                     std::pair{"T48b a newer names file is never written", std::wstring(L"refarter game names 2\ngame \"C:\\\\Games\\\\X\\\\x.exe\" \"Q\"\n")},
                                     std::pair{"T48c a names file with a relative path is never written", std::wstring(L"refarter game names 1\ngame \"x.exe\" \"Q\"\n")}}) {
        const std::string name = row;
        run(row, Setup{.games = list({x60}), .gameNamesText = text}, [&, name](AppTestAccess& h) {
            const auto bytes = fileBytes(gameNamesPath());
            check(!h.app->gameNamesReadable_, name + ": the file is unreadable");
            check(h.chooseFor(L"x game"), name + ": precondition: the page is on X");
            check(!h.shown(RenameGame), name + ": no pencil");
            check(caption(h) == L"settings for · x game", name + ": the usual name is shown");
            h.app->answerName_ = [](const std::wstring&) { return std::optional<std::wstring>(L"Q2"); };
            SendMessageW(h.app->window_, WM_COMMAND, MAKEWPARAM(RenameGame, BN_CLICKED), reinterpret_cast<LPARAM>(h.app->control(RenameGame)));
            pump();
            h.app->answerName_ = nullptr;
            h.app->quit();
            pump();
            check(fileBytes(gameNamesPath()) == bytes, name + ": the file is untouched, even after a forced click and quitting");
        });
    }

    // Choosing custom again must preserve a valid bitrate that has not yet been saved.
    run("T38 choosing custom again keeps a bitrate that could not be saved",
        Setup{.games = list({gameX(GameVideo{.quality = Quality::Custom, .bitrateMbps = 40})})}, [](AppTestAccess& h) {
        // X custom at 40, 70 typed, the file locked so leaving the field fails; custom picked again from the menu.
        check(h.chooseFor(L"x game") && h.text(Bitrate) == L"40", "T38 precondition: the page shows X's 40");
        check(h.lockGameFile(), "T38 precondition: the locked file refuses a replace");
        h.type(Bitrate, L"70");
        h.leave(Bitrate);
        check(h.app->typedBitrate_ == 70, "T38 precondition: 70 is still typed after the failed save");
        h.menu = [](HMENU shown) { return itemStarting(shown, L"custom"); };
        h.app->chooseQuality();
        h.menu = nullptr;
        check(h.app->typedBitrate_ == 70 && h.text(Bitrate) == L"70", "T38 70 is still typed");
        CloseHandle(h.lock);
        h.lock = INVALID_HANDLE_VALUE;
        h.leave(Bitrate);
        check(h.stored(xPath) && h.stored(xPath)->bitrateMbps == 70, "T38 70 is stored once saving works");
    });

    run("W1 existing installations do not welcome", Setup{}, [](AppTestAccess& h) {
        check(!h.app->welcome_, "W1 existing file is not a fresh install");
    });
    run("W2 fresh hidden startup persists the welcome", Setup{.fresh = true}, [](AppTestAccess& h) {
        check(h.app->welcome_ && h.app->dialogOpen(), "W2 fresh startup blocks other dialogs");
        check(h.app->hotkeys_[saveShortcut].key == VK_F23 && h.app->hotkeys_[screenshotShortcut].key == VK_F24,
            "W2 the fixture keeps the real default shortcuts free");
        h.app->autoCaptureOn_ = h.app->autoCapturePrompt_ = true;
        const auto context = h.app->autoContext();
        check(!context.autoOn && !context.prompt, "W2 automatic capture and its offers wait");
        h.app->autoCaptureOn_ = h.app->autoCapturePrompt_ = false;
        h.app->startupTray_ = true; // The state run() sets after a successful hidden sign-in launch.
        h.app->saveSettings();
        check(readSettings(h.app->config_.values()).welcomePending, "W2 hidden startup persists pending");
        h.close(); h.fresh = false;
        check(h.open() && h.app->welcome_, "W2 restart still welcomes");
    });
    // Rows W4-W6 come from the review of PR 178 at 765d17e.
    run("W4 errors from the welcome screen are not held back", Setup{.welcomePending = true}, [](AppTestAccess& h) {
        check(h.app->welcome_ && h.app->dialogOpen(), "W4 questions and what's new still wait");
        check(!h.app->modalOpen(), "W4 queued errors (a refused clip folder) do not wait for the welcome screen");
        h.app->asking_ = true;
        check(h.app->modalOpen(), "W4 a real dialog still holds them");
        h.app->asking_ = false;
    });
    run("W5 an unanswered welcome survives closing and quitting", Setup{.fresh = true}, [](AppTestAccess& h) {
        check(h.app->welcome_ && !h.app->startupTray_, "W5 precondition: an ordinary start shows the welcome");
        h.app->quit(); // Also what closing the window does without the tray; closing to the tray saves nothing.
        pump();
        check(readSettings(h.app->config_.values()).welcomePending, "W5 quitting keeps it pending");
        h.close(); h.fresh = false;
        check(h.open() && h.app->welcome_, "W5 the next start welcomes again");
        h.app->finishWelcome(false);
        check(!readSettings(h.app->config_.values()).welcomePending, "W5 skipping answers it");
    });
    run("W6 the welcome screen's shortcut note is the one Settings shows", Setup{.welcomePending = true}, [](AppTestAccess& h) {
        h.app->hotkeys_[saveShortcut] = {0, 'F'};
        check(h.app->registerHotkey(saveShortcut, h.app->hotkeys_[saveShortcut]), "W6 precondition: a plain key is watched");
        check(h.app->shortcutNote(saveShortcut) == L"works while replay runs, still types in other apps" &&
            !h.app->shortcutAlert(saveShortcut), "W6 a plain key warns that it still types");
        h.app->assigning_ = true;
        h.app->assigningShortcut_ = saveShortcut;
        check(h.app->shortcutNote(saveShortcut) == L"esc cancels · backspace resets" && h.app->shortcutAlert(saveShortcut),
            "W6 assigning shows the Settings hint");
        h.app->refreshControls();
        check(h.text(WelcomeShortcut) == L"save shortcut · press a new key", "W6 the button asks for a key as Settings does");
        h.app->heldModifiers_ = MOD_CONTROL;
        h.app->refreshControls();
        check(h.text(WelcomeShortcut) == h.text(AssignHotkey) && h.text(WelcomeShortcut) != L"save shortcut · press a new key",
            "W6 the button shows the held keys as Settings does");
        h.app->heldModifiers_ = 0;
        h.app->assigning_ = false;
        h.app->hotkeyRegistered_[saveShortcut] = false;
        check(h.app->shortcutNote(saveShortcut) == L"taken by another app · assign another", "W6 no save button to point to");
    });
    for (const bool start : {false, true}) for (const bool automatic : {false, true}) {
        run("W3 start and skip preserve their choices", Setup{.welcomePending = true}, [=](AppTestAccess& h) {
            h.app->state_.updateCheck = UpdateChoice::Ask;
            h.app->welcomeAutomatic_ = automatic;
            h.app->welcomeUpdates_ = false; // Never start a network request in this fixture.
            h.app->assigning_ = true;
            h.app->assigningShortcut_ = saveShortcut;
            const auto keys = h.app->hotkeys_[saveShortcut];
            const int seconds = h.app->state_.replaySeconds;
            const bool startup = h.app->state_.startWithWindows, tray = h.app->state_.closeToTray;
            check(h.app->welcomeSeconds_ == seconds && h.app->welcomeStartup_ == (startup && !h.app->startupOtherCopy_) &&
                h.app->welcomeTray_ == tray && !h.app->welcomeStartupChosen_,
                "W3 the new rows open on the current values");
            h.app->welcomeSeconds_ = seconds == 60 ? 30 : 60;
            const int chosen = h.app->welcomeSeconds_;
            h.app->welcomeTray_ = !tray;
            // Moved as a startup read would, never flipped by the user: start must leave the real Run entry alone.
            h.app->welcomeStartup_ = !h.app->welcomeStartup_;
            h.app->finishWelcome(start);
            check(h.app->state_.replaySeconds == (start ? chosen : seconds), "W3 only start applies the replay length");
            check(h.app->state_.closeToTray == (start ? !tray : tray), "W3 only start applies close to tray");
            const auto saved = readSettings(h.app->config_.values());
            check(saved.replaySeconds == h.app->state_.replaySeconds && saved.closeToTray == h.app->state_.closeToTray,
                "W3 the length and close to tray are saved");
            check(h.app->state_.startWithWindows == startup, "W3 a startup switch the user never flipped changes nothing");
            if (h.app->state_.closeToTray != tray) { // Put the tray back as the fixture had it.
                h.app->state_.closeToTray = tray;
                if (!tray) h.app->removeTrayIcon();
                h.app->saveSettings();
            }
            check(!h.app->welcome_ && !h.app->assigning_, "W3 welcome and unfinished assignment end");
            check(h.app->hotkeys_[saveShortcut].key == keys.key, "W3 cancelling assignment keeps shortcut");
            check(h.app->autoCaptureOn_ == (start && automatic) && h.app->autoCapturePrompt_ == (start && automatic), "W3 only start applies capture choice");
            check(h.app->state_.updateCheck == (start ? UpdateChoice::Off : UpdateChoice::Ask), "W3 skip does not opt in");
            check(h.app->state_.page == (start && !automatic ? Page::Recording : Page::Library), "W3 destination follows choice");
            check(!readSettings(h.app->config_.values()).welcomePending, "W3 answered screen is persisted");
            h.app->autoCaptureOn_ = h.app->autoCapturePrompt_ = false;
            h.app->auto_.updateHook();
            h.close(); check(h.open() && !h.app->welcome_, "W3 restart does not repeat welcome");
        });
    }

    run("T49 icon and meter tooltip registration survives layout and DPI changes", Setup{}, [](AppTestAccess& h) {
        const HWND tips = h.app->tooltips_;
        check(tips != nullptr, "T49 tooltip window exists");
        for (const int id : {SidebarToggle, Library, Recording, Settings, SaveReplay, StartReplay, SortClips,
                             GroupFolders, LargeTiles, SmallTiles, OpenFolder, DesktopReset, MicrophoneReset,
                             ReplayReset, ResolutionReset, FrameRateReset, QualityReset, BitrateReset, CodecReset, RenameGame}) {
            TOOLINFOW tool{toolInfoSize};
            tool.hwnd = h.app->window_;
            tool.uId = reinterpret_cast<UINT_PTR>(h.app->control(id));
            check(SendMessageW(tips, TTM_GETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&tool)) != FALSE,
                  "T49 every icon, reset and rename tool was accepted");
            check((tool.uFlags & TTF_IDISHWND) != 0, "T49 button tools retain HWND identity");
        }
        h.app->recording_ = capture::RecorderState::Buffering;
        h.app->state_.sidebarCollapsed = true;
        h.app->layout();
        const auto before = SendMessageW(tips, TTM_GETTOOLCOUNT, 0, 0);
        h.app->updateMeterTooltip();
        h.app->updateMeterTooltip();
        check(h.app->meterToolsAdded_ == 3, "T49 both meter registrations succeeded");
        check(SendMessageW(tips, TTM_GETTOOLCOUNT, 0, 0) == before, "T49 layout never duplicates meter tools");
        const auto dpi = h.app->dpi_;
        h.app->dpi_ = 192; // Exercise the real styling path without changing Windows display settings.
        h.app->createFonts();
        RECT margin{};
        SendMessageW(tips, TTM_GETMARGIN, 0, reinterpret_cast<LPARAM>(&margin));
        check(margin.left == 18 && margin.top == 8 && margin.right == 18 && margin.bottom == 8,
              "T49 tooltip padding follows 200 percent DPI");
        check(reinterpret_cast<HFONT>(SendMessageW(tips, WM_GETFONT, 0, 0)) == h.app->fontOf(Font::Small),
              "T49 tooltip uses the recreated small font");
        h.app->dpi_ = dpi;
        h.app->createFonts();
    });

    run("T50 locked rows relay once and preserve per-game heading and enable rules",
        Setup{.games = list({gameX(GameVideo{.quality = Quality::Custom, .bitrateMbps = 30})})}, [](AppTestAccess& h) {
        check(h.chooseFor(L"x game"), "T50 choose the game's own custom quality");
        check(IsWindowEnabled(h.app->control(Bitrate)), "T50 own custom bitrate is enabled with a global preset");
        h.app->recording_ = capture::RecorderState::Buffering;
        h.app->refreshControls();
        h.app->refreshPages();
        check(!IsWindowEnabled(h.app->control(Bitrate)), "T50 replay locks the game's bitrate");
        check(!h.app->lockedToolRects_.empty(), "T50 real locked rows register rectangles");
        RelayTrace trace;
        check(SetWindowSubclass(h.app->tooltips_, traceRelay, 99, reinterpret_cast<DWORD_PTR>(&trace)) != FALSE,
              "T50 trace the real tooltip relay");
        for (const int id : {QualityPreset, Bitrate, BitrateReset}) {
            RECT rect{};
            GetWindowRect(h.app->control(id), &rect);
            MapWindowPoints(nullptr, h.app->window_, reinterpret_cast<POINT*>(&rect), 2);
            const POINT point{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
            check(h.app->lockedTooltipAt(point), "T50 disabled control belongs to a locked row");
            trace = {};
            SendMessageW(h.app->window_, WM_MOUSEMOVE, 0, MAKELPARAM(point.x, point.y));
            check(trace.count == 1 && trace.target == h.app->window_, "T50 one move has one parent-row target, including resets");
        }
        RemoveWindowSubclass(h.app->tooltips_, traceRelay, 99);
        for (UINT i = 0; i < static_cast<UINT>(SendMessageW(h.app->tooltips_, TTM_GETTOOLCOUNT, 0, 0)); ++i) {
            TOOLINFOW tool{toolInfoSize};
            if (SendMessageW(h.app->tooltips_, TTM_ENUMTOOLSW, i, reinterpret_cast<LPARAM>(&tool)) &&
                !(tool.uFlags & TTF_IDISHWND) && h.app->isLockedTool(tool.uId))
                check(!(tool.uFlags & TTF_SUBCLASS), "T50 locked rows never install a second relay");
        }
        for (const UINT dpi : {96u, 192u}) {
            h.app->dpi_ = dpi;
            h.app->createFonts();
            h.app->layout();
            for (const auto& section : h.app->pages_[static_cast<size_t>(Page::Recording)]) {
                if (section.aside.empty() || !section.lockNoteWidth) continue;
                check(section.width - 8 - section.titleWidth - section.lockNoteWidth - 10 > 0,
                      "T50 scope aside and lock note reserve separate heading areas");
            }
        }
        h.app->recording_ = capture::RecorderState::Stopped;
        h.app->refreshPages();
        check(std::wstring(h.app->lockedToolText()).starts_with(L"clear the retained replay"), "T50 retained replay has the right action");
        h.app->recording_ = capture::RecorderState::Off;
        h.app->busy_ = true;
        h.app->refreshControls();
        h.app->refreshPages();
        check(h.app->lockedToolRects_.empty(), "T50 a non-replay busy lock has no replay tooltip");
        for (const auto& section : h.app->pages_[static_cast<size_t>(Page::Recording)])
            check(section.lockNoteWidth == 0, "T50 off has no locked heading notes");
    });

    for (const bool desired : {false, true}) {
        run("W7 welcome preserves a foreign startup entry that appeared meanwhile", Setup{.welcomePending = true}, [=](AppTestAccess& h) {
            h.app->welcomeAutomatic_ = h.app->welcomeUpdates_ = false;
            h.app->welcomeStartupChosen_ = true;
            h.app->welcomeStartup_ = desired;
            const std::wstring other = L"\"C:\\Games\\Other\\Refarter.exe\" --startup";
            check(writeStartup(other, h.startupTestKey.c_str()) == ERROR_SUCCESS, "W7 foreign entry appears after last read");
            h.app->finishWelcome(true);
            check(readStartup(h.startupTestKey.c_str()).command == other, "W7 foreign entry is unchanged");
            check(h.app->startupOtherCopy_, "W7 fresh ownership is shown");
        });
    }
    run("W8 an already-satisfied startup choice stays on", Setup{.welcomePending = true}, [](AppTestAccess& h) {
        h.app->welcomeAutomatic_ = h.app->welcomeUpdates_ = false;
        h.app->welcomeStartupChosen_ = h.app->welcomeStartup_ = true;
        const auto own = startupCommand(executablePath());
        check(writeStartup(own, h.startupTestKey.c_str()) == ERROR_SUCCESS, "W8 own entry appears after last read");
        h.app->finishWelcome(true);
        check(own && readStartup(h.startupTestKey.c_str()).command == *own && h.app->state_.startWithWindows,
              "W8 ON did not invert the new entry");
    });
    run("W9 an explicit off choice removes only this copy's new entry", Setup{.welcomePending = true}, [](AppTestAccess& h) {
        h.app->welcomeAutomatic_ = h.app->welcomeUpdates_ = false;
        h.app->welcomeStartupChosen_ = true;
        h.app->welcomeStartup_ = false;
        check(writeStartup(startupCommand(executablePath()), h.startupTestKey.c_str()) == ERROR_SUCCESS, "W9 own entry appeared");
        h.app->finishWelcome(true);
        check(readStartup(h.startupTestKey.c_str()).command.empty() && !h.app->state_.startWithWindows, "W9 explicit OFF applied freshly");
    });
    run("W10 a failed preference write rolls back the registration", Setup{}, [](AppTestAccess& h) {
        const HANDLE held = CreateFileW(configPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        check(held != INVALID_HANDLE_VALUE, "W10 lock the scratch ini against replacement");
        if (held == INVALID_HANDLE_VALUE) return;
        check(!h.app->applyStartupChoice(true), "W10 saving the preference fails");
        check(readStartup(h.startupTestKey.c_str()).command.empty() && !h.app->state_.startWithWindows, "W10 new registration rolled back");
        CloseHandle(held);
    });
    if (const auto monitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY)) run("W11 pending welcome blocks sign-in monitor replay",
        Setup{.welcomePending = true, .signInReplay = true, .sourceMonitor = monitorDevice(monitor)},
        [](AppTestAccess& h) {
        check(h.app->welcome_ && h.app->state_.signInReplay && h.app->state_.startWithWindows && h.app->target_.source.monitor,
              "W11 an eligible registered monitor startup is waiting on welcome");
        check(h.recorder->starts.empty() && !h.app->signInReplay_, "W11 no unattended start before answering welcome");
    });
    else std::cout << "skip  W11 no display monitor\n";

    run("W12 Settings still toggles this copy on and off", Setup{}, [](AppTestAccess& h) {
        check(h.app->toggleStartWithWindows() && h.app->state_.startWithWindows, "W12 Settings enables startup");
        check(readStartup(h.startupTestKey.c_str()).command == startupCommand(executablePath()).value_or(L""), "W12 correct executable");
        check(h.app->toggleStartWithWindows() && !h.app->state_.startWithWindows, "W12 Settings disables startup");
        check(readStartup(h.startupTestKey.c_str()).command.empty(), "W12 registration removed");
    });
    run("W13 an explicitly chosen on value survives an entry removed meanwhile", Setup{.welcomePending = true}, [](AppTestAccess& h) {
        const auto own = startupCommand(executablePath());
        check(writeStartup(own, h.startupTestKey.c_str()) == ERROR_SUCCESS, "W13 initially registered");
        h.app->refreshStartup();
        h.app->welcomeAutomatic_ = h.app->welcomeUpdates_ = false;
        h.app->welcomeStartupChosen_ = h.app->welcomeStartup_ = true;
        check(writeStartup(std::nullopt, h.startupTestKey.c_str()) == ERROR_SUCCESS, "W13 removed since last read");
        h.app->finishWelcome(true);
        check(own && readStartup(h.startupTestKey.c_str()).command == *own, "W13 explicit ON reapplied from fresh state");
    });

    std::cout << (failures ? std::to_string(failures) + " check(s) failed\n" : std::string("all rows pass\n"));
    return failures ? 1 : 0;
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
