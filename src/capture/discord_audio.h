#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace refarter::capture {

// Discord clients whose voice chat gets its own track: Discord, PTB, Canary and Vesktop.
bool isDiscordExecutable(std::wstring_view name);

struct ProcessEntry {
    DWORD pid = 0;
    DWORD parent = 0;
    std::wstring name; // Executable file name.
};
// The topmost Discord process in a process list: its tree plays the voice chat. 0 if none.
DWORD discordRoot(const std::vector<ProcessEntry>& processes);
// discordRoot of the running processes.
DWORD findDiscordProcess();

// Whether Discord plays (or has played since it started) on this output endpoint.
// A chosen output records everything on it, so Discord there cannot be split out.
// Asks Windows once; call on an MTA thread when needed, never on a timer.
bool discordPlaysOn(const std::wstring& endpointId);

// Whether `pid` is `root` or one of its descendants in a process list.
bool inProcessTree(const std::vector<ProcessEntry>& processes, DWORD root, DWORD pid);

// Where a game's process tree plays: on a chosen output, verifiably somewhere else, or not known.
enum class GameOutput { Unknown, Here, Elsewhere };
GameOutput gamePlaysOn(const std::wstring& endpointId, DWORD gameRoot);
// Whether the game's sound is already inside desktop audio (and so must not be summed into the mix again). Only a
// chosen output that verifiably does not carry the game makes it an independent source; unknown counts as contained.
inline bool gameInDesktopAudio(bool systemDefaultDesktop, GameOutput output) {
    return systemDefaultDesktop || output != GameOutput::Elsewhere;
}
// Where the game played for a save: a live answer when Windows gives one, else the last one seen while recording (the
// game and its window are often gone by the time the retained replay is saved), else unknown.
inline GameOutput knownGameOutput(GameOutput live, GameOutput lastSeen) {
    return live != GameOutput::Unknown ? live : lastSeen;
}

// Knows which Discord process is running while replay buffers, without polling:
// Windows reports each new audio session on the outputs (Discord starting to play
// sound), and a kernel wait reports its exit. `changed` runs on a Windows thread
// with the new process() whenever it changes. Construct and destroy on an MTA thread.
class DiscordWatch {
public:
    explicit DiscordWatch(std::function<void(DWORD)> changed);
    ~DiscordWatch();
    DiscordWatch(const DiscordWatch&) = delete;
    DiscordWatch& operator=(const DiscordWatch&) = delete;

    // Discord's topmost process ID, or 0 while it is not running.
    DWORD process() const { return pid_; }

private:
    struct Guard;
    struct Sessions;
    static void CALLBACK exitedCallback(PVOID context, BOOLEAN);
    void check();
    void exited();

    std::function<void(DWORD)> changed_;
    std::shared_ptr<Guard> guard_; // Callbacks run under its lock, and only while it points here.
    std::unique_ptr<Sessions> sessions_;
    std::atomic<DWORD> pid_ = 0;
    HANDLE process_ = nullptr;
    HANDLE wait_ = nullptr;
};

} // namespace refarter::capture
