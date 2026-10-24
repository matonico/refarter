#include "player/clip_player.h"
#include "player/audio_output.h"
#include "player/audio_sources.h"
#include "player/clip_trim.h"
#include "player/media.h"
#include "recycle_bin.h"
#include "clip_list.h"
#include "trim_follow_up.h"
#include "player/renderer.h"
#include "player/timeline.h"
#include "theme.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <shellapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>

namespace refarter::player {

namespace {
constexpr wchar_t className[] = L"RefarterClipPlayer";
constexpr wchar_t renameClass[] = L"RefarterRenameBox";
constexpr UINT statusChanged = WM_APP + 20;
constexpr UINT trimFinished = WM_APP + 21;
constexpr UINT_PTR hideTimer = 1, toastTimer = 2, progressTimer = 3;

enum Kind { Play, Pause, Toggle, Seek, Scrub, StepForward, StepBack, Track, Thumbnails };
struct Command {
    int kind = Play;
    double value = 0;
    int count = 0, width = 0, height = 0;
};

bool softwareOnly() {
    wchar_t value[8]{};
    return GetEnvironmentVariableW(L"REFARTER_PLAYER_SOFTWARE", value, 8) && value[0] == L'1';
}

// The seek bar, the trim strip and its handles: pressing there drags the playhead or a handle.
bool onTimeline(Part part) {
    return part == Part::Seek || part == Part::Timeline || part == Part::InHandle || part == Part::OutHandle;
}

double now() {
    static const double frequency = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return static_cast<double>(f.QuadPart); }();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return counter.QuadPart / frequency;
}
} // namespace

// Decoding, timing and drawing for one open clip, on its own thread. The window
// sends commands and its view; the engine reports a Status and posts
// statusChanged when something the window reacts to has changed.
struct ClipPlayer::Engine {
    HWND window;
    std::filesystem::path path; // Changed by a rename, under `mutex`, only once the clip is open.
    std::mutex mutex;
    View view;
    bool viewChanged = true;
    std::deque<Command> commands;
    std::shared_ptr<const ClipInfo> info = std::make_shared<const ClipInfo>();
    Playback playback;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<bool> stop = false;
    std::thread thread;

    std::mutex thumbnailMutex;
    std::vector<std::pair<double, winrt::com_ptr<ID3D11Texture2D>>> thumbnailTextures;
    std::thread thumbnailThread;
    std::atomic<bool> thumbnailStop = false;

    Engine(HWND window, std::filesystem::path path, const View& view) : window(window), path(std::move(path)), view(view) {
        thread = std::thread([this] { run(); });
    }
    ~Engine() {
        stop = true;
        SetEvent(wake);
        if (thread.joinable()) thread.join();
        CloseHandle(wake);
    }
    void push(Command command) {
        { std::lock_guard lock(mutex); commands.push_back(command); }
        SetEvent(wake);
    }
    void setView(const View& next) {
        { std::lock_guard lock(mutex); view = next; viewChanged = true; }
        SetEvent(wake);
    }
    // The open file was renamed: decoders already reading it carry on, later ones open the new name.
    void rename(std::filesystem::path to) {
        std::lock_guard lock(mutex);
        path = std::move(to);
    }
    std::shared_ptr<const ClipInfo> infoSnapshot() {
        std::lock_guard lock(mutex);
        return info;
    }
    Playback playbackSnapshot() {
        std::lock_guard lock(mutex);
        return playback;
    }
    void stopThumbnails() {
        thumbnailStop = true;
        if (thumbnailThread.joinable()) thumbnailThread.join();
        thumbnailStop = false;
        std::lock_guard lock(thumbnailMutex);
        thumbnailTextures.clear();
    }
    void startThumbnails(ID3D11Device* device, double duration, int count, UINT width, UINT height) {
        stopThumbnails();
        if (count <= 0 || !width || !height || duration <= 0) return;
        winrt::com_ptr<ID3D11Device> shared;
        shared.copy_from(device);
        std::filesystem::path clip;
        { std::lock_guard lock(mutex); clip = path; }
        thumbnailThread = std::thread([this, shared, clip, duration, count, width, height] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            {
                // A second decoder of its own: playback never waits for thumbnails.
                MediaFile file;
                if (file.open(clip, {shared.get(), false, softwareOnly()})) {
                    VideoConverter converter(shared.get());
                    for (int i = 0; i < count && !thumbnailStop && !stop; ++i) {
                        const double time = (i + 0.5) * duration / count;
                        const auto picture = file.keyframe(time);
                        auto texture = createPictureTexture(shared.get(), width, height);
                        if (!picture || !texture || !converter.convert(picture.frame.get(), texture.get())) continue;
                        { std::lock_guard lock(thumbnailMutex); thumbnailTextures.emplace_back(time, std::move(texture)); }
                        SetEvent(wake);
                    }
                }
            }
            winrt::uninit_apartment();
        });
    }

    void run();
};

void ClipPlayer::Engine::run() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    // The clip's facts are published as one immutable object, only when they change.
    ClipInfo local;
    Playback state;
    const auto publishInfo = [&] {
        auto shared = std::make_shared<const ClipInfo>(local);
        std::lock_guard lock(mutex);
        info = std::move(shared);
    };
    const auto publish = [&](bool notify) {
        { std::lock_guard lock(mutex); playback = state; }
        if (notify) PostMessageW(window, statusChanged, 0, 0);
    };
    {
        Renderer renderer;
        Painter painter;
        MediaFile media;
        AudioOutput audio;
        std::wstring error;
        // Refarter clips switch their sources (desktop, microphone, discord, game) on and off;
        // other files pick one track.
        std::vector<std::wstring> names;
        AudioSources sources;
        std::vector<bool> on;
        bool silent = false; // Every source is off: the decoders keep running (clock, A/V sync) but the output is muted.
        const auto describeAudio = [&] {
            if (sources.switchable()) {
                local.tracks = sources.names;
                const auto sounding = effectiveOn(sources, on); // The game is on while desktop is.
                local.checked = sounding;
                local.included = includedIn(sources, on);
                local.toggles = true; // One icon switch per source; no pill text.
                // A trim keeps what plays: every track while all sources are on.
                const bool all = std::find(sounding.begin(), sounding.end(), false) == sounding.end();
                local.trimTracks = all ? std::vector<int>{} : trimKeepFor(sources, on); // The game track stays even with desktop on.
                local.trimApart = all ? std::vector<int>{} : trimApartFor(sources, on);
                local.trimMix = all ? false : tracksFor(sources, on).size() > 1;
                local.trimNone = allOff(sources, on);
                local.trimNames.clear();
                for (std::size_t i = 0; i < sounding.size() && !all; ++i)
                    if (std::find(local.trimTracks.begin(), local.trimTracks.end(), sources.tracks[i]) != local.trimTracks.end()) {
                        std::string name; // desktop, microphone, discord, game: ASCII.
                        for (const wchar_t c : sources.names[i]) name += static_cast<char>(c);
                        local.trimNames.push_back(std::move(name));
                    }
                return;
            }
            const int track = media.audioTrack();
            local.tracks = names;
            local.checked.assign(names.size(), false);
            if (track >= 0) local.checked[track] = true;
            local.audioLabel = track >= 0 ? names[track] : L"";
        };
        if (!renderer.create(window, error)) {
            local.error = error;
        } else if (!media.open(path, {renderer.device(), true, softwareOnly()})) {
            local.error = media.error();
        } else {
            local.ready = true;
            local.duration = media.duration();
            local.frameDuration = media.frameDuration();
            local.keyframes = media.keyframes();
            for (int t = 0; t < media.audioTracks(); ++t) names.push_back(media.audioTrackName(t));
            sources = audioSources(names);
            on.assign(sources.tracks.size(), true);
            // Open on what all-on plays: the stored mix, or without one the sources summed (the media starts on one track).
            if (sources.switchable() && !media.selectAudio(tracksFor(sources, on))) media.selectAudio(std::vector<int>{sources.tracks.front()});
            describeAudio();
        }
        View shown = view;
        Picture current, next;
        bool playing = false, dirty = true, stale = false;
        // Any audio track will do: audioTrack() is -1 while several tracks are summed.
        const bool hasAudio = local.ready && media.audioTracks() > 0 && audio.open();
        bool audioClock = hasAudio;
        double position = 0, wallBase = 0, wallStart = now();
        std::vector<Thumbnail> thumbnails;
        std::vector<float> samples(4096 * 2);
        // What the last drawn pass showed, to tell whether another pass would look different.
        TimelineScale drawnSeek, drawnStrip;
        int drawnPixel = -1;
        long long drawnKey = -1;
        const auto timeKey = [&] { return std::llround(position * (shown.editing ? 10 : 1)); };

        const auto clock = [&] {
            if (!playing) return position;
            return audioClock ? audio.clock() : wallBase + (now() - wallStart);
        };
        const auto seekTo = [&](double time, bool onFrame) {
            time = std::clamp(time, 0.0, local.duration);
            current = media.seek(time);
            next = media.next();
            position = onFrame && current ? current.time : time;
            if (hasAudio) audio.flush(position);
            audioClock = hasAudio && audio.opened();
            wallBase = position;
            wallStart = now();
            stale = false;
            dirty = true;
        };
        const auto setPlaying = [&](bool on) {
            if (!local.ready || on == playing) return;
            if (on) {
                const double end = shown.editing ? shown.out : local.duration;
                if (position >= end - local.frameDuration / 2) seekTo(shown.editing ? shown.in : 0, false);
                else if (stale) seekTo(position, true); // Frame steps moved the picture, not the audio.
                wallBase = position;
                wallStart = now();
            } else {
                position = std::min(clock(), local.duration);
                wallBase = position;
            }
            playing = on;
            if (hasAudio) audio.play(on);
            dirty = true;
        };
        // The graphics device was removed or reset: nothing new can be shown, so say so, pause and stop drawing.
        const auto deviceLost = [&] {
            local.error = L"the graphics device was reset · open the clip again";
            publishInfo();
            setPlaying(false);
            state.playing = playing;
            publish(true);
        };
        if (local.ready) {
            seekTo(0, false);
            setPlaying(true); // A double click means "watch this".
        }
        state.playing = playing;
        publishInfo();
        publish(true);

        while (!stop) {
            std::deque<Command> batch;
            {
                std::lock_guard lock(mutex);
                batch.swap(commands);
                if (viewChanged) {
                    shown = view;
                    viewChanged = false;
                    dirty = true;
                }
            }
            const float level = shown.muted ? 0.f : shown.volume / 100.f;
            audio.setVolume(silent ? 0.f : level * level); // Closer to how loudness is heard.
            const bool wasPlaying = playing;
            bool infoChanged = false, labelChanged = false;
            // Only the newest seek of a batch matters while scrubbing.
            int lastSeek = -1;
            for (int i = 0; i < static_cast<int>(batch.size()); ++i)
                if (batch[i].kind == Seek || batch[i].kind == Scrub) lastSeek = i;
            for (int i = 0; i < static_cast<int>(batch.size()) && local.ready; ++i) {
                const auto& command = batch[i];
                switch (command.kind) {
                case Play: setPlaying(true); break;
                case Pause: setPlaying(false); break;
                case Toggle: setPlaying(!playing); break;
                case Seek: case Scrub:
                    if (i == lastSeek) seekTo(command.value, false);
                    break;
                case StepForward:
                    setPlaying(false);
                    if (next) {
                        current = std::move(next);
                        next = media.next();
                        position = current.time;
                        stale = dirty = true;
                    }
                    break;
                case StepBack:
                    setPlaying(false);
                    if (current) {
                        current = media.previous(current.time);
                        next = media.next();
                        if (current) position = current.time;
                        if (hasAudio) audio.flush(position);
                        stale = dirty = true; // Resume realigns audio to the stepped picture.
                    }
                    break;
                case Track: {
                    const auto item = static_cast<std::size_t>(command.value);
                    auto wanted = sources.switchable() ? effectiveOn(sources, on) : on;
                    std::vector<int> tracks{static_cast<int>(item)};
                    if (sources.switchable()) {
                        if (item >= wanted.size() || includedIn(sources, on)[item]) break; // Part of desktop audio.
                        wanted[item] = !wanted[item];
                        tracks = tracksFor(sources, wanted); // Empty while every source is off.
                    }
                    const auto previous = media.audioTrack();
                    if (!sources.switchable() && tracks.front() == previous) break; // Already playing.
                    if (sources.switchable() && tracks.empty()) {
                        on = wanted; // All off: keep decoding what was selected and mute it.
                        silent = true;
                    } else if (!media.selectAudio(tracks)) {
                        // Restore a working selection; while silent only what all-on plays (the stored mix, or the summed sources) is a safe choice.
                        media.selectAudio(sources.switchable() ? (silent ? tracksFor(sources, std::vector<bool>(sources.tracks.size(), true)) : tracksFor(sources, on))
                                                               : std::vector<int>{previous});
                    } else {
                        on = wanted;
                        silent = false;
                    }
                    const auto audioLabel = local.audioLabel;
                    const auto checkedBefore = local.checked;
                    describeAudio();
                    infoChanged = true;
                    labelChanged = audioLabel != local.audioLabel || checkedBefore != local.checked;
                    const bool resume = playing;
                    setPlaying(false);
                    seekTo(position, false);
                    setPlaying(resume);
                    break;
                }
                case Thumbnails:
                    thumbnails.clear();
                    startThumbnails(renderer.device(), local.duration, command.count,
                                    static_cast<UINT>(command.width), static_cast<UINT>(command.height));
                    dirty = true;
                    break;
                }
            }

            if (playing) {
                if (hasAudio) {
                    while (audio.queued() < 0.25) {
                        const auto frames = media.audio(samples.data(), 4096);
                        if (!frames) break;
                        audio.write(samples.data(), frames);
                    }
                    // Audio ended (or the device went away): the wall clock takes over.
                    if (audioClock && (!audio.opened() || (media.audioEnded() && audio.queued() <= 0))) {
                        wallBase = audio.clock();
                        wallStart = now();
                        audioClock = false;
                    }
                }
                const double time = clock();
                bool advanced = false;
                while (next && next.time <= time + 0.002) {
                    current = std::move(next);
                    next = media.next();
                    advanced = true;
                }
                position = std::min(time, local.duration);
                if (shown.editing && time >= shown.out) {
                    setPlaying(false);
                    position = shown.out;
                } else if (!next && time >= std::max(local.duration, current ? current.time + local.frameDuration : 0.0) - 0.001) {
                    setPlaying(false);
                    position = local.duration;
                }
                // Draw again only when the picture, the seek bar by a pixel or the time text would look different.
                const TimelineScale scale = shown.editing ? drawnStrip : drawnSeek;
                if (advanced || static_cast<int>(std::lround(scale.x(position))) != drawnPixel || timeKey() != drawnKey) dirty = true;
            }

            {
                std::lock_guard lock(thumbnailMutex);
                for (auto& [time, texture] : thumbnailTextures) {
                    thumbnails.push_back({time, renderer.wrap(texture.get())});
                    dirty = true;
                }
                thumbnailTextures.clear();
            }

            state.position = position;
            state.playing = playing;
            if (infoChanged) publishInfo();
            publish(playing != wasPlaying || labelChanged);

            // Draw only for a reason, and never while the window cannot be seen.
            const HWND root = GetAncestor(window, GA_ROOT);
            const bool visible = IsWindowVisible(window) && !IsIconic(root);
            if (dirty && visible && renderer.device() && !renderer.lost()) {
                RECT client{};
                GetClientRect(window, &client);
                const bool sized = client.right > 0 && client.bottom > 0;
                if (sized && renderer.resize(client.right, client.bottom)) {
                    const auto layout = computeLayout(static_cast<float>(client.right), static_cast<float>(client.bottom), shown, local);
                    drawnSeek = {layout.seek.left, layout.seek.right - layout.seek.left, local.duration};
                    drawnStrip = {layout.strip.left, layout.strip.right - layout.strip.left, local.duration};
                    drawnPixel = static_cast<int>(std::lround((shown.editing ? drawnStrip : drawnSeek).x(position)));
                    drawnKey = timeKey();
                    ID2D1Bitmap1* picture = current ? renderer.picture(current.frame.get(),
                        static_cast<UINT>(layout.video.right - layout.video.left), static_cast<UINT>(layout.video.bottom - layout.video.top)) : nullptr;
                    painter.draw(renderer.begin(), renderer.text(), renderer.factory(), layout, shown, local, state, picture, thumbnails);
                    renderer.end(); // Waits for the vertical blank: this paces playback drawing.
                }
                if (renderer.lost()) deviceLost();
                dirty = false;
            } else if (playing) {
                // Until the next picture is due (at most a vertical blank); a minimised window only keeps
                // the audio queue (a quarter of a second) fed.
                DWORD wait = 100;
                if (visible) {
                    wait = 16;
                    if (next) wait = static_cast<DWORD>(std::clamp((next.time - clock()) * 1000 - 1, 1.0, 16.0));
                }
                WaitForSingleObject(wake, wait);
            } else {
                WaitForSingleObject(wake, INFINITE);
            }
        }
        stopThumbnails();
        thumbnails.clear();
        current = {};
        next = {};
    }
    winrt::uninit_apartment();
}

ClipPlayer::ClipPlayer() = default;

ClipPlayer::~ClipPlayer() {
    if (renameBox_) DestroyWindow(renameBox_); // No rename on the way out: the box forgets its field first.
    if (renameFont_) DeleteObject(renameFont_);
    if (renameBrush_) DeleteObject(renameBrush_);
    trimCancel_ = true; // A copy being written is removed rather than left half done.
    if (trimThread_.joinable()) trimThread_.join();
    engine_.reset();
}

HWND ClipPlayer::create(HWND parent, int id, Callbacks callbacks) {
    if (!theme::registerWindowClass(className, proc, CS_DBLCLKS)) return nullptr;
    callbacks_ = std::move(callbacks);
    window_ = CreateWindowExW(0, className, L"clip player", WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), this);
    view_.scale = static_cast<float>(GetDpiForWindow(parent)) / 96.f;
    return window_;
}

void ClipPlayer::open(const std::filesystem::path& path, int volume, const std::wstring& date, bool muted) {
    close();
    path_ = path;
    const float scale = view_.scale;
    const bool fullscreen = view_.fullscreen;
    view_ = {};
    view_.scale = scale;
    view_.fullscreen = fullscreen;
    view_.volume = std::clamp(volume, 0, 100);
    view_.muted = muted; // Set before the engine starts, so a muted clip never blips.
    setTitle(path);
    view_.date = date;
    if (date.empty()) {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        SYSTEMTIME now{};
        GetLocalTime(&now);
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) view_.date = formatClipDate(info.ftLastWriteTime, now);
    }
    thumbnailsFor_ = 0;
    engine_ = std::make_unique<Engine>(window_, path, view_);
}

void ClipPlayer::setTitle(const std::filesystem::path& path) {
    view_.title = clipDisplayName(path);
    view_.titleWidth = 0;
    if (const HDC dc = GetDC(window_)) {
        // At 96 dpi: the layout scales it. GDI and DirectWrite differ by a pixel or two, which a click does not mind.
        const HFONT font = theme::font(96, 17, FW_SEMIBOLD), old = static_cast<HFONT>(SelectObject(dc, font));
        SIZE extent{};
        if (GetTextExtentPoint32W(dc, view_.title.c_str(), static_cast<int>(view_.title.size()), &extent)) view_.titleWidth = static_cast<float>(extent.cx);
        SelectObject(dc, old);
        DeleteObject(font);
        ReleaseDC(window_, dc);
    }
}

void ClipPlayer::close() {
    finishRename(false);
    ++session_;
    KillTimer(window_, hideTimer);
    engine_.reset(); // Joins the engine: decoders, GPU surfaces and the audio device are released.
    view_.editing = false;
    InvalidateRect(window_, nullptr, FALSE);
}

void ClipPlayer::setDpi(UINT dpi) {
    view_.scale = static_cast<float>(dpi) / 96.f;
    publish();
}

void ClipPlayer::setFullscreen(bool on) {
    view_.fullscreen = on;
    view_.controls = true;
    if (on) showControls();
    else KillTimer(window_, hideTimer);
    publish();
}

void ClipPlayer::setCompressionBusy(bool busy, const std::filesystem::path& clip) {
    compressClip_ = busy ? clip : std::filesystem::path{};
    compressBusy_ = busy;
    if (!view_.editing || view_.compressBusy == busy) return;
    view_.compressBusy = busy;
    publish();
}

void ClipPlayer::showToast(const std::wstring& text, ToastKind kind, bool hold) {
    view_.toast = text;
    view_.toastKind = kind;
    if (hold || text.empty()) KillTimer(window_, toastTimer);
    else SetTimer(window_, toastTimer, 4000, nullptr);
    publish();
}

void ClipPlayer::publish() {
    if (engine_) engine_->setView(view_);
}

std::shared_ptr<const ClipInfo> ClipPlayer::info() const {
    static const auto none = std::make_shared<const ClipInfo>();
    return engine_ ? engine_->infoSnapshot() : none;
}

Playback ClipPlayer::playback() const { return engine_ ? engine_->playbackSnapshot() : Playback{}; }

Layout ClipPlayer::layout() const { return layout(*info()); }

Layout ClipPlayer::layout(const ClipInfo& info) const {
    RECT client{};
    GetClientRect(window_, &client);
    return computeLayout(static_cast<float>(client.right), static_cast<float>(client.bottom), view_, info);
}

void ClipPlayer::command(int kind, double value) {
    if (engine_) engine_->push({kind, value});
}

void ClipPlayer::seek(double time, bool scrub) { command(scrub ? Scrub : Seek, time); }

void ClipPlayer::step(int frames) { command(frames > 0 ? StepForward : StepBack); }

void ClipPlayer::setVolume(int volume, bool remember) {
    view_.volume = std::clamp(volume, 0, 100);
    if (view_.volume > 0) view_.muted = false;
    publish();
    if (remember && callbacks_.volume) callbacks_.volume(view_.volume);
}

void ClipPlayer::toggleFullscreen() {
    if (callbacks_.fullscreen) callbacks_.fullscreen(!view_.fullscreen);
}

void ClipPlayer::showControls() {
    if (!view_.controls) {
        view_.controls = true;
        publish();
    }
    if (view_.fullscreen) SetTimer(window_, hideTimer, 2500, nullptr);
}

void ClipPlayer::requestThumbnails() {
    const auto current = info();
    if (!view_.editing || !current->ready || !engine_) return;
    const auto l = layout(*current);
    float w = 0, h = 0;
    const int count = thumbnailCount(l, *current, w, h);
    const int key = count * 10000 + static_cast<int>(w);
    if (key == thumbnailsFor_) return;
    thumbnailsFor_ = key;
    Command request{Thumbnails};
    request.count = count;
    request.width = static_cast<int>(w);
    request.height = static_cast<int>(h);
    engine_->push(request);
}

void ClipPlayer::startTrim() {
    if (followUpBlocksTrim()) { showToast(L"finish or cancel compression before starting another trim", ToastKind::Notice, false); return; }
    const auto current = info();
    if (!current->ready || view_.editing) return;
    view_.editing = true;
    view_.in = 0;
    view_.out = current->duration;
    view_.compressBusy = compressBusy_;
    view_.compressCopy = compressDefault_;
    publish();
    requestThumbnails();
}

void ClipPlayer::endTrim() {
    if (!view_.editing) return;
    view_.editing = false;
    publish();
}

// Cancel, Esc and the back button: leave trim mode, and stop a copy that is still being written (it is removed).
void ClipPlayer::leaveTrim() {
    if (view_.editing && view_.saving && !view_.compressing && trimThread_.joinable()) trimCancel_ = true;
    endTrim();
}

void ClipPlayer::save(bool replace) {
    if (followUpBlocksTrim()) { showToast(L"finish or cancel compression before saving this trim", ToastKind::Notice, false); return; }
    const auto current = info();
    if (!view_.editing || view_.saving || !current->ready) return;
    if (trimThread_.joinable()) { showToast(L"the last trim is still being saved", ToastKind::Notice, false); return; } // Still being written.
    if (replace && compressBusy_ && compressClip_ == path_) { // The file is in use by the compression.
        view_.toast = L"finish or cancel compression before replacing this clip";
        view_.toastKind = ToastKind::Notice;
        SetTimer(window_, toastTimer, 3000, nullptr);
        publish();
        return;
    }
    const auto range = snapTrim(current->keyframes, view_.in, view_.out, current->duration);
    const TrimAudio audio{current->trimTracks, current->trimMix, current->trimNames, current->trimApart, current->trimNone};
    // The whole clip is worth saving again only without some of its audio.
    if (range.start <= 0.0005 && range.end >= current->duration - current->frameDuration / 2 && !audio.changesAudio()) {
        view_.toast = L"move a handle to choose the part to keep";
        view_.toastKind = ToastKind::Notice;
        SetTimer(window_, toastTimer, 3000, nullptr);
        publish();
        return;
    }
    replace_ = replace;
    followUp_ = view_.compressCopy && compressCopyAvailable(view_.compressBusy);
    view_.saving = true;
    view_.progress = 0;
    view_.toast.clear();
    trimProgress_ = 0;
    trimCancel_ = false;
    trimError_.clear();
    trimmed_.clear();
    trimmedIdentity_ = {};
    const auto source = path_;
    trimSource_ = source;
    trimSession_ = session_;
    trimThread_ = std::thread([this, source, range, audio, window = window_] {
        const auto result = trimClip(source, range, trimProgress_, trimCancel_, audio);
        trimError_ = result.ok ? L"" : result.error;
        trimmed_ = result.ok ? result.path : std::filesystem::path{};
        trimmedIdentity_ = result.identity;
        PostMessageW(window, trimFinished, 0, 0);
    });
    SetTimer(window_, progressTimer, 100, nullptr);
    publish();
}

std::filesystem::path ClipPlayer::replaceOriginal(bool keepCopyName) {
    const auto original = path_, copy = trimmed_;
    const int volume = view_.volume;
    const bool muted = view_.muted;
    if (callbacks_.replacing) callbacks_.replacing(original, keepCopyName ? copy : original);
    engine_.reset(); // Releases the file so Windows can move it.
    // To the recycle bin or not at all: where no bin exists the original stays in place and is reported.
    const bool recycled = recycleOnly({original}, GetAncestor(window_, GA_ROOT)) == 0 && !std::filesystem::exists(original);
    const bool moved = recycled && (keepCopyName || MoveFileExW(copy.c_str(), original.c_str(), MOVEFILE_WRITE_THROUGH));
    const auto result = moved && !keepCopyName ? original : copy;
    open(keepCopyName ? copy : moved || !recycled ? original : copy, volume, {}, muted);
    view_.toastKind = moved ? ToastKind::Done : ToastKind::Partial;
    // Both failures keep the copy under its own name; say where the original is.
    view_.toast = moved ? L"saved · the original is in the recycle bin" :
        recycled ? L"saved as " + copy.filename().wstring() + L" · the original is in the recycle bin, the copy could not take its name" :
                   L"saved as " + copy.filename().wstring() + L" · the original could not be replaced";
    publish();
    if (callbacks_.saved) callbacks_.saved(result, original);
    return result;
}

void ClipPlayer::trimCompressionProgress(float progress, std::uint64_t session) {
    if (!followUpPending_ || session_ != session || !view_.compressing) return;
    view_.progress = progress;
    publish();
}

// Only the clip the pending trim belongs to is held back; other clips trim as usual.
bool ClipPlayer::followUpBlocksTrim() const {
    return followUpPending_ && path_ == followUpSource_;
}

std::wstring ClipPlayer::finishTrimCompression(const std::filesystem::path& result, bool replace, std::uint64_t session,
                                               bool takeOriginalName, std::filesystem::path* kept) {
    followUpPending_ = false;
    followUpSource_.clear();
    if (session_ != session) return {};
    view_.saving = view_.compressing = false;
    if (result.empty()) { publish(); return {}; }
    if (replace) {
        trimmed_ = result;
        const auto placed = replaceOriginal(!takeOriginalName);
        if (kept) *kept = placed;
        return view_.toastKind == ToastKind::Partial ? view_.toast : std::wstring{};
    }
    const auto source = path_;
    open(result, view_.volume, {}, view_.muted);
    if (callbacks_.saved) callbacks_.saved(result, source);
    return {};
}

// The name is typed into a small popup over the title: the picture's composition surface covers child windows.
LRESULT CALLBACK ClipPlayer::renameProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* player = reinterpret_cast<ClipPlayer*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        player = static_cast<ClipPlayer*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(player));
    }
    if (!player) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window, &ps);
        RECT client{};
        GetClientRect(window, &client);
        theme::fill(dc, client, theme::surface);
        theme::roundRect(dc, client, MulDiv(7, static_cast<int>(player->view_.scale * 96), 96), theme::background, theme::accent);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        const auto dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, theme::ink);
        SetBkColor(dc, theme::background);
        return reinterpret_cast<LRESULT>(player->renameBrush_);
    }
    case WM_COMMAND:
        // Clicking elsewhere or switching apps renames, as in the library.
        if (reinterpret_cast<HWND>(lParam) == player->renameEdit_ && HIWORD(wParam) == EN_KILLFOCUS) player->finishRename(true);
        return 0;
    case WM_WINDOWPOSCHANGING:
        // Hidden by anything but finishRename (the main window closing hides it first): cancel, never rename on the way out.
        if ((reinterpret_cast<WINDOWPOS*>(lParam)->flags & SWP_HIDEWINDOW) && window == player->renameBox_)
            player->renameBox_ = player->renameEdit_ = nullptr;
        break;
    case WM_DESTROY:
        // Also when the main window goes: the field must not rename on its way out.
        if (window == player->renameBox_) player->renameBox_ = player->renameEdit_ = nullptr;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void ClipPlayer::beginRename() {
    if (renameEdit_ || path_.empty() || !window_) return;
    if (trimBusy()) { showToast(L"finish the trim before renaming this clip", ToastKind::Notice, false); return; }
    if (compressBusy_ && compressClip_ == path_) { showToast(L"finish or cancel compression before renaming this clip", ToastKind::Notice, false); return; }
    if (!theme::registerWindowClass(renameClass, renameProc, 0)) return;
    const UINT dpi = static_cast<UINT>(std::lround(view_.scale * 96));
    if (renameFont_) DeleteObject(renameFont_);
    renameFont_ = theme::font(dpi, 17, FW_SEMIBOLD); // The title's size and weight.
    if (!renameBrush_) renameBrush_ = CreateSolidBrush(theme::background);
    renameBox_ = CreateWindowExW(WS_EX_TOOLWINDOW, renameClass, L"rename clip", WS_POPUP | WS_CLIPCHILDREN, 0, 0, 0, 0,
        GetAncestor(window_, GA_ROOT), nullptr, GetModuleHandleW(nullptr), this);
    if (!renameBox_) return;
    renameEdit_ = CreateWindowExW(0, L"EDIT", view_.title.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0,
        renameBox_, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!renameEdit_) { DestroyWindow(renameBox_); return; }
    SendMessageW(renameEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(renameFont_), FALSE);
    SendMessageW(renameEdit_, EM_SETLIMITTEXT, 200, 0);
    showControls(); // The title bar stays while the name is typed.
    placeRename();
    ShowWindow(renameBox_, SW_SHOW);
    SetFocus(renameEdit_);
    SendMessageW(renameEdit_, EM_SETSEL, 0, -1);
}

// Over the title, from its left edge to the rename button; follows a resize or fullscreen.
void ClipPlayer::placeRename() {
    if (!renameBox_) return;
    const auto l = layout();
    const float s = view_.scale;
    const int left = static_cast<int>((view_.fullscreen ? 20.f : 8.f) * s), right = static_cast<int>(l.rename.left - 8 * s);
    const int height = static_cast<int>(34 * s), top = static_cast<int>((l.head.top + l.head.bottom) / 2) - height / 2;
    POINT origin{left, top};
    ClientToScreen(window_, &origin);
    const int width = std::max(static_cast<int>(80 * s), right - left);
    SetWindowPos(renameBox_, HWND_TOP, origin.x, origin.y, width, height, SWP_NOACTIVATE);
    const int radius = static_cast<int>(14 * s);
    SetWindowRgn(renameBox_, CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius), TRUE); // The window owns the region.
    int text = static_cast<int>(22 * s);
    if (const HDC dc = GetDC(renameEdit_)) {
        const auto old = SelectObject(dc, renameFont_);
        TEXTMETRICW metrics{};
        if (GetTextMetricsW(dc, &metrics)) text = metrics.tmHeight;
        SelectObject(dc, old);
        ReleaseDC(renameEdit_, dc);
    }
    const int inset = static_cast<int>(8 * s);
    SetWindowPos(renameEdit_, nullptr, inset, (height - text) / 2, width - 2 * inset, text, SWP_NOZORDER | SWP_NOACTIVATE);
}

bool ClipPlayer::renameKey(WPARAM key) {
    if (!renameEdit_ || GetFocus() != renameEdit_) return false;
    if (key == VK_RETURN || key == VK_TAB) finishRename(true);
    else if (key == VK_ESCAPE) finishRename(false);
    else return false;
    return true;
}

void ClipPlayer::back() {
    if (renameEdit_) finishRename(true); // As a click elsewhere: the typed name is kept.
    else if (view_.fullscreen) toggleFullscreen();
    else if (view_.editing) leaveTrim();
    else if (callbacks_.back) callbacks_.back();
}

void ClipPlayer::finishRename(bool commit) {
    if (!renameEdit_) return;
    wchar_t text[256]{};
    GetWindowTextW(renameEdit_, text, static_cast<int>(std::size(text)));
    const HWND box = std::exchange(renameBox_, nullptr); // Losing focus below must not rename again.
    renameEdit_ = nullptr;
    if (GetFocus() && IsChild(box, GetFocus())) SetFocus(window_);
    DestroyWindow(box);
    if (commit) commitRename(text);
}

// The decoders share the open file for renaming, so playback carries on untouched: only the title changes.
void ClipPlayer::commitRename(const std::wstring& text) {
    const auto name = trimClipName(text);
    if (name.empty() || name == clipDisplayName(path_) || !callbacks_.rename) return;
    switch (checkClipName(name)) {
    case ClipNameProblem::InvalidCharacter: showToast(L"a clip name can't contain \\ / : * ? \" < > |", ToastKind::Partial, false); return;
    case ClipNameProblem::Reserved: showToast(L"windows reserves that name · try another", ToastKind::Partial, false); return;
    default: break;
    }
    if (trimBusy()) return;
    const bool ready = info()->ready; // The engine has opened the file and reads the old name no more.
    std::filesystem::path renamed;
    auto problem = callbacks_.rename(path_, name, renamed);
    if (!renamed.empty() && ready) {
        path_ = renamed;
        engine_->rename(renamed);
        setTitle(renamed);
    } else if (!renamed.empty()) {
        const auto date = view_.date; // open() clears the view, so it must not be passed by reference.
        open(renamed, view_.volume, date, view_.muted); // Still opening, or not playable: tried again under the new name.
    }
    if (problem.empty() && renamed.empty()) problem = L"the name did not change";
    const bool failed = problem == renameFailedText; // Access denied or in use: a real error; a refused name stays partial.
    if (!problem.empty() && problem.back() == L'.') problem.pop_back(); // Toasts end without a full stop.
    showToast(problem.empty() ? L"renamed" : problem, problem.empty() ? ToastKind::Done : failed ? ToastKind::Error : ToastKind::Partial, false);
}

void ClipPlayer::activate(Part part) {
    const auto current = info();
    switch (part) {
    case Part::Play: case Part::Stage: command(Toggle); break;
    case Part::Mute:
        view_.muted = !view_.muted || view_.volume == 0;
        if (!view_.muted && view_.volume == 0) setVolume(50, true);
        publish();
        break;
    case Part::Trim: startTrim(); break;
    case Part::Cancel:
        if (view_.compressing && callbacks_.cancelTrimCompression) callbacks_.cancelTrimCompression();
        else leaveTrim();
        break;
    case Part::Save: save(false); break;
    case Part::Replace: save(true); break;
    case Part::CompressCopy:
        if (view_.compressBusy) showToast(compressionBusyHint, ToastKind::Notice, false);
        else { view_.compressCopy = !view_.compressCopy; publish(); }
        break;
    case Part::Folder:
        command(Pause);
        if (callbacks_.folder) callbacks_.folder(path_);
        break;
    case Part::ErrorOpen:
        command(Pause);
        if (callbacks_.external) callbacks_.external(path_);
        break;
    case Part::Fullscreen: toggleFullscreen(); break;
    case Part::Rename: case Part::Title: beginRename(); break;
    case Part::Back:
        if (view_.editing) leaveTrim(); // As Esc does while trimming.
        else if (callbacks_.back) callbacks_.back();
        break;
    case Part::Source0: case Part::Source1: case Part::Source2: case Part::Source3: case Part::Source4: case Part::Source5:
        if (const auto item = static_cast<std::size_t>(sourceIndex(part)); item < current->included.size() && current->included[item]) {
            // Cannot be taken out of desktop audio.
            showToast(L"game sound is part of desktop audio · turn desktop off to hear the game alone", ToastKind::Notice, false);
            break;
        }
        command(Track, sourceIndex(part)); // The engine flips that source (kept or left out when trimming).
        break;
    case Part::Track: {
        if (current->tracks.size() < 2 || !callbacks_.choose) break;
        const auto l = layout(*current);
        POINT at{static_cast<LONG>(l.track.left), static_cast<LONG>(l.track.bottom + 4 * view_.scale)};
        ClientToScreen(window_, &at);
        const int chosen = callbacks_.choose(current->tracks, current->checked, at);
        if (chosen >= 0) command(Track, chosen);
        break;
    }
    default: break;
    }
}

void ClipPlayer::drag(float x, float) {
    const auto current = info();
    const auto l = layout(*current);
    switch (view_.pressed) {
    case Part::Seek: case Part::Timeline: {
        double time = timeAt(l, view_, *current, x);
        if (view_.editing) time = std::clamp(time, view_.in, view_.out);
        view_.hoverTime = time;
        seek(time, true);
        publish();
        break;
    }
    case Part::InHandle: {
        // The start follows keyframes: what is shown is exactly where the copy begins.
        const double time = keyframeAtOrBefore(current->keyframes, std::min(timeAt(l, view_, *current, x), view_.out - 0.5));
        if (time != view_.in) { view_.in = std::max(0.0, time); seek(view_.in, true); }
        publish();
        break;
    }
    case Part::OutHandle: {
        const double time = trimEndAt(timeAt(l, view_, *current, x), view_.in, current->duration);
        if (time != view_.out) { view_.out = time; seek(time, true); }
        publish();
        break;
    }
    case Part::Volume: {
        const float level = (x - l.volume.left) / std::max(1.f, l.volume.right - l.volume.left);
        setVolume(static_cast<int>(std::lround(std::clamp(level, 0.f, 1.f) * 100)), false);
        break;
    }
    default: break;
    }
}

LRESULT CALLBACK ClipPlayer::proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* player = reinterpret_cast<ClipPlayer*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        player = static_cast<ClipPlayer*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        player->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(player));
    }
    return player ? player->handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

LRESULT ClipPlayer::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        // Behind the composition surface: the window colour for its rounded
        // corners, and the error text if the GPU surface could not be made.
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window_, &ps);
        RECT client{};
        GetClientRect(window_, &client);
        theme::fill(dc, client, view_.fullscreen ? RGB(0, 0, 0) : theme::background);
        const auto current = info();
        if (!current->error.empty()) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, theme::muted);
            DrawTextW(dc, current->error.c_str(), -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        EndPaint(window_, &ps);
        return 0;
    }
    case WM_SIZE:
        publish();
        placeRename();
        if (view_.editing) requestThumbnails();
        return 0;
    case statusChanged: {
        if (view_.editing) requestThumbnails();
        if (!playback().playing) showControls(); // Paused in fullscreen: controls stay.
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    }
    case trimFinished: {
        if (trimThread_.joinable()) trimThread_.join();
        KillTimer(window_, progressTimer);
        const bool sameSession = session_ == trimSession_;
        const bool cancelled = trimCancel_; // Cancel or Esc while the copy was being written.
        if (sameSession) view_.saving = false;
        if (callbacks_.trimEnded) callbacks_.trimEnded();
        std::filesystem::path done;
        if (cancelled) {
            // Delete only the output identified before publication, never a replacement at its pathname.
            const bool cleanupFailed = !trimmed_.empty() && !discardTemporaryClip(trimmed_, trimmedIdentity_);
            if (!cleanupFailed) trimmed_.clear();
            const auto notice = cleanupFailed ? L"trim cancelled · completed copy could not be removed" : L"trim cancelled";
            if (sameSession) {
                view_.toast = notice;
                view_.toastKind = cleanupFailed ? ToastKind::Error : ToastKind::Notice;
            } else if (cleanupFailed && callbacks_.trimFailed) {
                callbacks_.trimFailed(notice);
            }
        } else if (!trimmed_.empty() && followUp_) {
            // Keep the original and its trim handles on screen until the compressed result is ready.
            done = trimmed_;
            followUpPending_ = true;
            followUpSource_ = trimSource_;
            if (sameSession) {
                view_.saving = view_.compressing = true;
                view_.progress = 0;
            }
            if (callbacks_.saved) callbacks_.saved(done, trimSource_);
        } else if (!trimmed_.empty() && replace_ && sameSession) {
            done = replaceOriginal();
            trimSession_ = session_; // The internal reopen still belongs to this trim.
        } else if (!trimmed_.empty()) {
            // The new clip opens here, playing. Once the user leaves, keep the saved copy without changing another
            // player session.
            if (sameSession) {
                open(trimmed_, view_.volume, {}, view_.muted);
                trimSession_ = session_; // The reopen still belongs to this trim (its compress & copy).
                view_.toast = L"saved as " + trimmed_.filename().wstring();
                view_.toastKind = ToastKind::Done;
            }
            if (callbacks_.saved) callbacks_.saved(trimmed_, trimSource_);
            done = trimmed_;
        } else if (sameSession) {
            view_.toast = trimError_;
            view_.toastKind = ToastKind::Error;
        } else if (callbacks_.trimFailed) {
            // The player was left: the library's note carries the failure.
            auto reason = trimError_;
            if (!reason.empty() && reason.back() == L'.') reason.pop_back(); // Notes end without a full stop.
            callbacks_.trimFailed(reason);
        }
        if (sameSession) {
            SetTimer(window_, toastTimer, 4000, nullptr);
            publish();
        }
        // After the toast timer, so a follow-up that holds the toast (progress) is not faded out.
        if (!done.empty() && callbacks_.trimSaved) callbacks_.trimSaved({done, trimSource_, followUp_, replace_, trimSession_});
        return 0;
    }
    case WM_TIMER:
        if (wParam == hideTimer) {
            KillTimer(window_, hideTimer);
            if (view_.fullscreen && playback().playing && view_.pressed == Part::None && !renameEdit_) {
                view_.controls = false;
                view_.hover = Part::None;
                publish();
            }
        } else if (wParam == toastTimer) {
            KillTimer(window_, toastTimer);
            view_.toast.clear();
            publish();
        } else if (wParam == progressTimer) {
            if (session_ == trimSession_) {
                view_.progress = trimProgress_;
                publish();
            }
        }
        return 0;
    case WM_MOUSEMOVE: {
        const float x = static_cast<float>(GET_X_LPARAM(lParam)), y = static_cast<float>(GET_Y_LPARAM(lParam));
        if (lParam != lastMove_) { lastMove_ = lParam; if (view_.fullscreen) showControls(); }
        if (!tracking_) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
            tracking_ = TrackMouseEvent(&track) != FALSE;
        }
        if (view_.pressed != Part::None) { drag(x, y); return 0; }
        const auto current = info();
        const auto l = layout(*current);
        const auto part = hitTest(l, view_, *current, x, y);
        const double hoverTime = part == Part::Seek || part == Part::Timeline ? timeAt(l, view_, *current, x) : -1;
        if (part != view_.hover || hoverTime != view_.hoverTime) {
            view_.hover = part;
            view_.hoverTime = hoverTime;
            publish();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (view_.pressed == Part::None && (view_.hover != Part::None || view_.hoverTime >= 0)) {
            view_.hover = Part::None;
            view_.hoverTime = -1;
            publish();
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            if (view_.fullscreen && !view_.controls) { SetCursor(nullptr); return TRUE; }
            const auto part = view_.pressed != Part::None ? view_.pressed : view_.hover;
            const bool handle = part == Part::InHandle || part == Part::OutHandle;
            const bool clickable = part != Part::None && part != Part::Stage;
            SetCursor(LoadCursorW(nullptr, handle ? IDC_SIZEWE : part == Part::Title ? IDC_IBEAM : clickable ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        SetFocus(window_);
        const float x = static_cast<float>(GET_X_LPARAM(lParam)), y = static_cast<float>(GET_Y_LPARAM(lParam));
        const auto current = info();
        const auto part = hitTest(layout(*current), view_, *current, x, y);
        if (view_.fullscreen && !view_.controls) { showControls(); return 0; }
        view_.pressed = part;
        view_.hover = part;
        SetCapture(window_);
        const bool timeline = onTimeline(part);
        if (timeline) {
            wasPlaying_ = playback().playing;
            if (wasPlaying_) command(Pause);
        }
        if (timeline || part == Part::Volume) drag(x, y);
        publish();
        return 0;
    }
    case WM_LBUTTONUP: {
        const float x = static_cast<float>(GET_X_LPARAM(lParam)), y = static_cast<float>(GET_Y_LPARAM(lParam));
        const auto pressed = view_.pressed;
        view_.pressed = Part::None;
        ReleaseCapture();
        const auto part = hitTest(layout(), view_, *info(), x, y);
        if (onTimeline(pressed)) {
            if (wasPlaying_ && pressed != Part::InHandle && pressed != Part::OutHandle) command(Play);
            wasPlaying_ = false;
        } else if (pressed == Part::Volume) {
            setVolume(view_.volume, true);
        } else if (pressed == part && part != Part::None) {
            activate(part);
        }
        view_.hover = part;
        publish();
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        const auto part = hitTest(layout(), view_, *info(), static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)));
        if (part == Part::Stage) {
            command(Toggle); // Undo the first click's pause, as video players do.
            toggleFullscreen();
        } else if (part != Part::None) {
            SendMessageW(window_, WM_LBUTTONDOWN, wParam, lParam); // Fast repeated clicks still count.
        }
        return 0;
    }
    case WM_CAPTURECHANGED: case WM_CANCELMODE:
        if (view_.pressed != Part::None) {
            view_.pressed = Part::None;
            publish();
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window_, &point);
        const int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
        const auto part = hitTest(layout(), view_, *info(), static_cast<float>(point.x), static_cast<float>(point.y));
        if (part == Part::Volume || part == Part::Mute) setVolume(view_.volume + notches * 5, true);
        else if (onTimeline(part)) {
            // Wheel over the timeline: frame by frame, or seconds with shift.
            if (GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) seek(playback().position - notches, false);
            else for (int i = 0; i < std::abs(notches); ++i) step(notches < 0 ? 1 : -1);
        }
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_KEYDOWN: {
        const auto current = playback();
        const double duration = info()->duration;
        const bool repeat = (lParam & (1 << 30)) != 0;
        if (view_.fullscreen) showControls();
        // Ctrl+C, Ctrl+F and the like belong to the system, not to the single-letter shortcuts.
        const bool modified = (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000);
        if (modified && wParam >= 'A' && wParam <= 'Z') return DefWindowProcW(window_, message, wParam, lParam);
        switch (wParam) {
        case VK_SPACE: case 'K': if (!repeat) command(Toggle); break;
        case VK_LEFT: if (view_.editing) step(-1); else seek(current.position - 5, false); break;
        case VK_RIGHT: if (view_.editing) step(1); else seek(current.position + 5, false); break;
        case 'J': seek(current.position - 10, false); break;
        case 'L': seek(current.position + 10, false); break;
        case VK_OEM_COMMA: step(-1); break;
        case VK_OEM_PERIOD: step(1); break;
        case VK_UP: setVolume(view_.volume + 5, true); break;
        case VK_DOWN: setVolume(view_.volume - 5, true); break;
        case 'M': if (!repeat) activate(Part::Mute); break;
        case 'F': if (!repeat) toggleFullscreen(); break;
        case 'T': if (!repeat) { if (view_.editing) endTrim(); else startTrim(); } break;
        case VK_F2: if (!repeat) beginRename(); break;
        case 'C': if (!repeat && view_.editing && !view_.saving) activate(Part::CompressCopy); break;
        case VK_HOME: seek(view_.editing ? view_.in : 0, false); break;
        case VK_END: seek(view_.editing ? view_.out : duration, false); break;
        case VK_RETURN: if (!repeat && view_.editing) save(false); break;
        case VK_ESCAPE: if (!repeat) back(); break;
        default: return DefWindowProcW(window_, message, wParam, lParam);
        }
        return 0;
    }
    case WM_SYSKEYDOWN: // Alt+← steps back, as in a browser.
        if (wParam == VK_LEFT && !(lParam & (1 << 30))) { back(); return 0; }
        break;
    case WM_SETFOCUS: case WM_KILLFOCUS:
        return 0;
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

} // namespace refarter::player
