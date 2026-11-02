#include "capture/track_workers.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <thread>

using refarter::capture::TrackWorkers;

// Fails the n-th allocation made by the arming thread while it is counting, once: the constructor's own thread
// starts, vector and callback copies. Other threads (the workers, the watchdog) are never counted.
namespace {
thread_local bool counting = false;
thread_local long seen = 0;
thread_local long failAt = 0;
std::atomic<int> entered{0}, left{0};
int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAILED: %s\n", what);
        ++failures;
    }
}
} // namespace

void* operator new(std::size_t size) {
    if (counting && ++seen == failAt) {
        failAt = 0;
        throw std::bad_alloc();
    }
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

int main() {
    // A regression ends in std::terminate (a joinable thread destroyed) or a hang: both fail the test.
    std::set_terminate([] {
        std::puts("FAILED: std::terminate (a started worker was not joined)");
        std::_Exit(42);
    });
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        std::puts("FAILED: timed out");
        std::_Exit(43);
    }).detach();

    const auto enter = [] { ++entered; };
    const auto leave = [] { ++left; };

    // Normal path: every worker runs the job once per round, and the first error reaches wait().
    {
        std::atomic<int> ran{0};
        TrackWorkers workers(3, enter, leave);
        workers.start([&](std::size_t) { ++ran; });
        workers.wait();
        check(ran == 3, "each worker ran the job once");
        workers.start([](std::size_t index) {
            if (index == 1) throw std::runtime_error("job");
        });
        bool rethrown = false;
        try {
            workers.wait();
        } catch (const std::runtime_error&) {
            rethrown = true;
        }
        check(rethrown, "a job's exception reaches wait()");
    }
    check(entered == 3 && left == 3, "normal path joins all three workers");

    // Fail each allocation of the constructor in turn. Whichever one fails after a worker has started, that
    // worker must have been told to finish and joined before the exception reaches the caller.
    bool startedBeforeFailure = false;
    bool completed = false;
    for (long k = 1; k <= 200 && !completed; ++k) {
        entered = 0;
        left = 0;
        bool threw = false;
        seen = 0;
        failAt = k;
        counting = true;
        try {
            TrackWorkers workers(4, enter, leave);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        counting = false;
        failAt = 0;
        check(entered == left, "every worker that started was joined");
        if (threw) {
            if (entered > 0) startedBeforeFailure = true;
            check(entered < 4, "a failed construction did not start every worker");
        } else {
            completed = true;
            check(entered == 4 && left == 4, "construction without a failure runs and joins all four");
        }
    }
    check(completed, "the loop reached an allocation count past the constructor's");
    check(startedBeforeFailure, "some allocation failed after a worker had started");

    std::puts(failures ? "track_workers_test: FAILED" : "track_workers_test: ok");
    return failures ? 1 : 0;
}
