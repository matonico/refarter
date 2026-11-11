#pragma once

#include <windows.h>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <utility>

namespace refarter {

// One detached worker thread with a single result, for the update check and the installer download. The worker owns
// shared state only, so cancelling or destroying the task never waits for it: a cancelled worker finishes on its own,
// stores nothing and posts nothing. The UI thread owns the task; the result arrives as `message` to `window`, then
// take() hands it over once.
template <class Result>
class BackgroundTask {
    struct Shared {
        std::mutex mutex;
        std::condition_variable wake;
        bool cancelled = false;
        std::optional<Result> result; // Set once, when the worker finishes uncancelled.
    };

public:
    // What the worker sees of its task.
    class Worker {
    public:
        bool cancelled() const { std::lock_guard lock(shared_.mutex); return shared_.cancelled; }
        // Waits `delay` unless cancelled first; true when cancelled.
        template <class Rep, class Period>
        bool waitFor(const std::chrono::duration<Rep, Period>& delay) const {
            std::unique_lock lock(shared_.mutex);
            return shared_.wake.wait_for(lock, delay, [&] { return shared_.cancelled; });
        }

    private:
        friend class BackgroundTask;
        explicit Worker(Shared& shared) : shared_(shared) {}
        Shared& shared_;
    };

    ~BackgroundTask() { cancel(); }

    // Replaces a task that has not finished. `work(const Worker&)` returns the Result on a new thread; without a
    // thread (std::system_error) nothing runs and running() stays false.
    template <class Work>
    void start(HWND window, UINT message, Work work) {
        cancel();
        auto shared = std::make_shared<Shared>();
        try {
            std::thread([shared, window, message, work = std::move(work)]() mutable {
                auto result = work(Worker(*shared));
                std::lock_guard lock(shared->mutex);
                if (shared->cancelled) return;
                shared->result = std::move(result);
                PostMessageW(window, message, 0, 0);
            }).detach();
            shared_ = std::move(shared);
        } catch (const std::system_error&) {
            // No thread: nothing runs this time; the next start tries again.
        }
    }

    void cancel() {
        if (!shared_) return;
        {
            std::lock_guard lock(shared_->mutex);
            shared_->cancelled = true;
        }
        shared_->wake.notify_all();
        shared_.reset(); // After the lock: this may be the last owner.
    }

    bool running() const { return shared_ != nullptr; } // Started and not yet taken.

    // The result of a finished task, once.
    std::optional<Result> take() {
        if (!shared_) return std::nullopt;
        std::optional<Result> result;
        {
            std::lock_guard lock(shared_->mutex);
            if (!shared_->result) return std::nullopt;
            result = std::move(shared_->result);
        }
        shared_.reset();
        return result;
    }

private:
    std::shared_ptr<Shared> shared_;
};

} // namespace refarter
