#include "recording_controller.h"
#include "capture/clip_folder.h"
#include "diagnostic_log.h"

namespace refarter {
RecordingController::~RecordingController() {
    close();
    if (worker_.joinable()) worker_.join();
}

void RecordingController::enqueue(std::function<void()> job) {
    std::lock_guard lock(mutex_);
    if (closing_) return;
    if (!worker_.joinable()) worker_ = std::thread([this] { run(); });
    jobs_.push_back(std::move(job));
    ready_.notify_one();
}

void RecordingController::run() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return exit_ || !jobs_.empty(); });
            if (exit_) break;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        job();
    }
    winrt::uninit_apartment();
}

void RecordingController::start(RecordingTarget target, capture::VideoSettings video,
                                 capture::AudioSettings audio, std::filesystem::path folderPath) {
    HANDLE startupCancel = nullptr;
    {
        std::lock_guard lock(mutex_);
        if (closing_) return;
        if (!startupCancel_) startupCancel_.attach(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        startupCancel = startupCancel_.get();
        if (startupCancel) ResetEvent(startupCancel);
    }
    if (!startupCancel) {
        notify_({RecordingEvent::Kind::Started, capture::RecorderState::Off, L"windows could not prepare recording startup", {}, false});
        return;
    }
    enqueue([this, target = std::move(target), video, audio, folderPath = std::move(folderPath), startupCancel] {
        diagnosticLog().write(DiagnosticEvent::StartRequested, S_OK, video.fps, static_cast<unsigned>(video.codec));
        if (target.source.window) diagnosticLog().write(DiagnosticEvent::GameTarget, S_OK, target.pid);
        RecordingEvent event{RecordingEvent::Kind::Started};
        try {
            DWORD pid = 0;
            if (target.source.window) GetWindowThreadProcessId(target.source.window, &pid);
            if (target.source.window && (!target.process || pid != target.pid ||
                    WaitForSingleObject(target.process.get(), 0) != WAIT_TIMEOUT)) {
                event.message = L"the selected game closed; choose it again";
            } else if (auto folderError = capture::prepareClipFolder(folderPath)) {
                event.message = *folderError;
            } else {
                if (!recorder_) recorder_ = std::make_unique<capture::Recorder>();
                auto error = recorder_->start(target.source, video, audio, [this] {
                    enqueue([this] {
                        if (recorder_ && recorder_->state() == capture::RecorderState::Stopped) {
                            diagnosticLog().write(DiagnosticEvent::ReplayStopped, S_OK, 2);
                            notify_({RecordingEvent::Kind::Stopped, capture::RecorderState::Stopped,
                                     recorder_->message() + L" · replay retained; save it before clearing"});
                        }
                    });
                }, [this, desktop = audio.desktop.enabled(), microphone = audio.microphone.enabled()] {
                    // Device threads only signal; the state is read here, after start has reported.
                    enqueue([this, desktop, microphone] {
                        if (!recorder_ || recorder_->state() != capture::RecorderState::Buffering) return;
                        const auto stats = recorder_->stats();
                        RecordingEvent event{RecordingEvent::Kind::Audio, capture::RecorderState::Buffering};
                        event.desktopMissing = desktop && !stats.desktopAudioActive;
                        event.microphoneMissing = microphone && !stats.micAudioActive;
                        event.discordUnavailable = stats.discordUnavailable;
                        notify_(std::move(event));
                    });
                }, startupCancel, target.process.get());
                event.retryableStart = recorder_->startRetryable();
                if (error) event.message = *error;
                else {
                    event.state = capture::RecorderState::Buffering;
                    event.message = L"buffering · " + std::to_wstring(recorder_->outputSize().width) + L" × " +
                        std::to_wstring(recorder_->outputSize().height) + L" · " + std::to_wstring(video.fps) + L" fps · " +
                        capture::codecName(video.codec) + L" · " + capture::describe(recorder_->bitrate());
                }
            }
        } catch (const winrt::hresult_error&) { event.message = L"windows could not open the recorder"; }
        catch (const std::exception&) { event.message = L"could not start replay; check available memory and the clip folder"; }
        event.success = event.state == capture::RecorderState::Buffering;
        if (event.success) diagnosticLog().write(DiagnosticEvent::CaptureStarted, S_OK,
            recorder_->outputSize().width, recorder_->outputSize().height);
        else diagnosticLog().write(DiagnosticEvent::StartFailed, S_OK, event.retryableStart);
        if (!event.success) recorder_.reset();
        notify_(std::move(event));
    });
}

void RecordingController::stop() {
    { std::lock_guard lock(mutex_); if (startupCancel_) SetEvent(startupCancel_.get()); }
    enqueue([this] {
        recorder_.reset(); // Includes pending saves and all device threads.
        diagnosticLog().write(DiagnosticEvent::ReplayStopped);
        notify_({RecordingEvent::Kind::Stopped, capture::RecorderState::Off, L"replay off · buffer cleared"});
    });
}

void RecordingController::pause() {
    enqueue([this] {
        RecordingEvent event{RecordingEvent::Kind::Stopped};
        event.sleeping = true;
        try {
            // Already retained just before the suspend (window closed, encoder failed): it is kept as it is, not reset.
            if (recorder_ && (recorder_->retain(L"paused for sleep") || recorder_->state() == capture::RecorderState::Stopped)) {
                event.state = capture::RecorderState::Stopped;
                event.message = L"paused for sleep · replay kept";
                diagnosticLog().write(DiagnosticEvent::ReplayStopped, S_OK, 2);
                notify_(std::move(event));
                return;
            }
        } catch (const std::exception&) {} // Falls back to a plain stop below.
        recorder_.reset(); // Nothing to keep (empty buffer): the same stop as before.
        event.message = L"replay paused for sleep";
        diagnosticLog().write(DiagnosticEvent::ReplayStopped);
        notify_(std::move(event));
    });
}

void RecordingController::save(std::filesystem::path folderPath, std::wstring name, std::int64_t requestedAt) {
    enqueue([this, folderPath = std::move(folderPath), name = std::move(name), requestedAt] {
        diagnosticLog().write(DiagnosticEvent::SaveRequested);
        RecordingEvent event{RecordingEvent::Kind::Saved};
        event.success = false;
        try {
            if (!recorder_) event.message = L"enable replay before saving a clip";
            else if (auto folderError = capture::prepareClipFolder(folderPath)) event.message = *folderError;
            else {
                const auto path = capture::nextClipPath(folderPath, name);
                if (path.empty()) event.message = L"could not save the clip · no free file name in the clip folder";
                else if (auto error = recorder_->saveClip(path, [this](auto error, const std::filesystem::path& saved) {
                    diagnosticLog().write(error ? DiagnosticEvent::SaveFailed : DiagnosticEvent::SaveFinished);
                    notify_({RecordingEvent::Kind::Saved, capture::RecorderState::Off,
                             error ? *error : L"saved · " + saved.filename().wstring(), saved, !error});
                }, requestedAt)) event.message = *error;
                else return;
            }
        } catch (const std::exception&) { event.message = L"could not save the clip; check available memory and storage"; }
        diagnosticLog().write(DiagnosticEvent::SaveFailed);
        notify_(std::move(event));
    });
}

void RecordingController::screenshot(std::filesystem::path folderPath, std::wstring name) {
    enqueue([this, folderPath = std::move(folderPath), name = std::move(name)] {
        RecordingEvent event{RecordingEvent::Kind::Screenshot};
        event.success = false;
        try {
            if (!recorder_) event.message = L"screenshots need replay running";
            else {
                // The picture is asked for first; the folder (beside the clips, out of the library's list) and the
                // name are prepared on the screenshot thread once it is copied.
                auto path = std::make_shared<std::filesystem::path>();
                const auto target = [folder = folderPath / L"screenshots", name, path](std::wstring& problem) -> std::filesystem::path {
                    try {
                        if (auto folderError = capture::prepareClipFolder(folder, capture::ClipFolderKind::Screenshots)) { problem = *folderError; return {}; }
                        *path = capture::nextClipPath(folder, name, L".png");
                        if (path->empty()) problem = L"screenshot not saved · no free file name in the screenshots folder";
                        return *path;
                    } catch (const std::exception&) {
                        problem = L"screenshot not saved · the screenshots folder could not be prepared";
                        return {};
                    }
                };
                auto error = recorder_->screenshot(target, [this, path](auto error) {
                    notify_({RecordingEvent::Kind::Screenshot, capture::RecorderState::Off,
                             error ? *error : L"screenshot saved · " + path->filename().wstring(), *path, !error});
                });
                if (!error) return;
                event.message = *error;
            }
        } catch (const std::exception&) { event.message = L"could not take a screenshot; check available memory and storage"; }
        notify_(std::move(event));
    });
}

void RecordingController::folder(std::filesystem::path path) {
    enqueue([this, path = std::move(path)] {
        std::optional<std::wstring> error;
        try { error = capture::prepareClipFolder(path); }
        catch (const std::exception&) { error = L"could not check the clip folder"; }
        notify_({RecordingEvent::Kind::Folder, capture::RecorderState::Off,
                 error ? *error : L"clip folder updated", path, !error});
    });
}

void RecordingController::probeCodecs() {
    enqueue([this] {
        RecordingEvent event{RecordingEvent::Kind::Codecs};
        try { event.codecs = capture::probeCodecs(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY)); }
        catch (const std::exception&) {} // No results: only h.264 stays selectable.
        notify_(std::move(event));
    });
}

void RecordingController::close() {
    std::lock_guard lock(mutex_);
    if (closing_) return;
    if (startupCancel_) SetEvent(startupCancel_.get());
    closing_ = true;
    if (!worker_.joinable()) {
        notify_({RecordingEvent::Kind::Closed});
        return;
    }
    jobs_.push_back([this] {
        recorder_.reset();
        diagnosticLog().write(DiagnosticEvent::RecorderClosed);
        {
            std::lock_guard lock(mutex_);
            exit_ = true;
        }
        notify_({RecordingEvent::Kind::Closed});
    });
    ready_.notify_one();
}
} // namespace refarter
