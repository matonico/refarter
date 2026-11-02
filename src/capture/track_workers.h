#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace refarter::capture {

// A fixed set of threads that run one job each, all at once, round after round (a save's AAC encoders, one per
// track). start() hands every worker the same job with its own index; wait() returns once all have finished and
// rethrows the first exception one of them threw. Call start() only after construction or wait().
// `enter` and `leave` run on each worker thread at its start and end (thread priority, COM apartment).
class TrackWorkers {
public:
    explicit TrackWorkers(std::size_t count, std::function<void()> enter = {}, std::function<void()> leave = {}) {
        try {
            threads_.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                threads_.emplace_back([this, index, enter, leave] {
                    if (enter) enter();
                    loop(index);
                    if (leave) leave();
                });
            }
        } catch (...) {
            // The destructor does not run for a constructor that throws, and a joinable std::thread being destroyed
            // ends the process: end the threads already started before the members they use go.
            finish();
            throw;
        }
    }
    // Lets a round in progress finish (its job may still use what it was given), then ends the threads.
    ~TrackWorkers() { finish(); }
    TrackWorkers(const TrackWorkers&) = delete;
    TrackWorkers& operator=(const TrackWorkers&) = delete;

    std::size_t size() const { return threads_.size(); }

    void start(std::function<void(std::size_t worker)> job) {
        {
            std::lock_guard lock(mutex_);
            job_ = std::move(job);
            busy_ = threads_.size();
            ++round_;
        }
        wake_.notify_all();
    }

    void wait() {
        std::unique_lock lock(mutex_);
        done_.wait(lock, [&] { return busy_ == 0; });
        if (auto error = std::exchange(error_, nullptr)) std::rethrow_exception(error);
    }

private:
    void finish() {
        {
            std::lock_guard lock(mutex_);
            quit_ = true;
        }
        wake_.notify_all();
        for (auto& thread : threads_) thread.join();
    }

    void loop(std::size_t index) {
        std::uint64_t seen = 0;
        std::unique_lock lock(mutex_);
        for (;;) {
            wake_.wait(lock, [&] { return quit_ || round_ != seen; });
            if (round_ == seen) return; // Quitting, nothing left to run.
            seen = round_;
            lock.unlock();
            std::exception_ptr error;
            try {
                job_(index); // Not replaced before every worker has finished (see start).
            } catch (...) {
                error = std::current_exception();
            }
            lock.lock();
            if (error && !error_) error_ = error;
            if (--busy_ == 0) done_.notify_all();
        }
    }

    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable wake_, done_;
    std::function<void(std::size_t)> job_;
    std::uint64_t round_ = 0;
    std::size_t busy_ = 0;
    std::exception_ptr error_;
    bool quit_ = false;
};

} // namespace refarter::capture
