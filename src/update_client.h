#pragma once

#include <windows.h>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace refarter {

// What a release check found. Newer carries the tag and the page to open; Failed covers every way of not getting
// an answer (offline, timeout, rate limit, a server error, unreadable JSON).
struct UpdateResult {
    enum class Outcome { Newer, Latest, NoRelease, Failed } outcome = Outcome::Failed;
    std::wstring tag; // Newer only.
    std::wstring url; // Newer only: the release page.
    bool manual = false; // The "check for updates" button asked, not the start-up check.
};

// One GET of the latest release from GitHub over WinHTTP, on a short-lived worker thread (update_check.h decides
// what the answer means). Nothing runs until start(): no timer, no polling, no thread at idle. The worker waits
// `delay` (cancellable), asks once, stores the result and posts `message` to the window; the UI thread then calls
// take(). The thread only touches its own shared state, so cancelling or destroying the client never waits for the
// network (a request in flight ends on its own within the timeouts and posts nothing).
class UpdateClient {
public:
    ~UpdateClient() { cancel(); }
    // Replaces a check that has not finished. `running` is the version to compare against ("0.5.1-alpha").
    void start(HWND window, UINT message, std::chrono::seconds delay, bool manual, std::wstring running);
    void cancel();
    bool running() const; // Started and not yet taken.
    std::optional<UpdateResult> take(); // The result of a finished check, once.

private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace refarter
