#include "discord_audio.h"

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <winrt/base.h>

#include <mutex>
#include <optional>
#include <unordered_map>

namespace refarter::capture {

bool isDiscordExecutable(std::wstring_view name) {
    for (const std::wstring_view known : {L"discord.exe", L"discordptb.exe", L"discordcanary.exe", L"vesktop.exe"}) {
        if (CompareStringOrdinal(name.data(), static_cast<int>(name.size()), known.data(), static_cast<int>(known.size()), TRUE) == CSTR_EQUAL)
            return true;
    }
    return false;
}

DWORD discordRoot(const std::vector<ProcessEntry>& processes) {
    std::unordered_map<DWORD, DWORD> discord; // Process ID to parent ID.
    for (const auto& process : processes)
        if (process.pid && isDiscordExecutable(process.name)) discord[process.pid] = process.parent;
    // Discord's helpers are its children; the process whose parent is not Discord owns the tree.
    DWORD root = 0;
    for (const auto& [pid, parent] : discord)
        if (!discord.contains(parent) && (!root || pid < root)) root = pid;
    return root;
}

namespace {
// Every running process with its parent; nothing when Windows cannot take the snapshot.
std::optional<std::vector<ProcessEntry>> runningProcesses() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;
    std::vector<ProcessEntry> processes;
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
        processes.push_back({entry.th32ProcessID, entry.th32ParentProcessID, entry.szExeFile});
    CloseHandle(snapshot);
    return processes;
}

// The audio sessions of one output. Throws when the output or its session list cannot be opened.
winrt::com_ptr<IAudioSessionEnumerator> audioSessions(const std::wstring& endpointId) {
    const auto enumerator = winrt::create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
    winrt::com_ptr<IMMDevice> output;
    winrt::com_ptr<IAudioSessionManager2> manager;
    winrt::com_ptr<IAudioSessionEnumerator> sessions;
    winrt::check_hresult(enumerator->GetDevice(endpointId.c_str(), output.put()));
    winrt::check_hresult(output->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, manager.put_void()));
    winrt::check_hresult(manager->GetSessionEnumerator(sessions.put()));
    return sessions;
}
} // namespace

DWORD findDiscordProcess() {
    const auto processes = runningProcesses();
    return processes ? discordRoot(*processes) : 0;
}

bool discordPlaysOn(const std::wstring& endpointId) {
    try {
        const auto sessions = audioSessions(endpointId);
        int count = 0;
        winrt::check_hresult(sessions->GetCount(&count));
        for (int i = 0; i < count; ++i) {
            winrt::com_ptr<IAudioSessionControl> session;
            AudioSessionState state = AudioSessionStateExpired;
            DWORD pid = 0;
            if (FAILED(sessions->GetSession(i, session.put())) || FAILED(session->GetState(&state)) ||
                state == AudioSessionStateExpired) continue;
            const auto owner = session.try_as<IAudioSessionControl2>();
            if (!owner || owner->GetProcessId(&pid) != S_OK || !pid) continue; // System sounds, shared sessions.
            const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!process) continue;
            wchar_t path[MAX_PATH];
            DWORD size = MAX_PATH;
            const bool named = QueryFullProcessImageNameW(process, 0, path, &size) != FALSE;
            CloseHandle(process);
            if (!named) continue;
            const std::wstring_view full(path, size);
            if (isDiscordExecutable(full.substr(full.find_last_of(L'\\') + 1))) return true;
        }
    } catch (...) {} // Unknown: assume it can be split out.
    return false;
}

bool inProcessTree(const std::vector<ProcessEntry>& processes, DWORD root, DWORD pid) {
    if (!root || !pid) return false;
    std::unordered_map<DWORD, DWORD> parents;
    for (const auto& process : processes) parents[process.pid] = process.parent;
    for (std::size_t depth = 0; pid && depth <= processes.size(); ++depth) {
        if (pid == root) return true;
        const auto found = parents.find(pid);
        if (found == parents.end() || found->second == pid) return false;
        pid = found->second;
    }
    return false;
}

GameOutput gamePlaysOn(const std::wstring& endpointId, DWORD gameRoot) {
    if (!gameRoot) return GameOutput::Unknown;
    try {
        const auto processes = runningProcesses();
        if (!processes) return GameOutput::Unknown;
        const auto sessions = audioSessions(endpointId);
        int count = 0;
        winrt::check_hresult(sessions->GetCount(&count));
        for (int i = 0; i < count; ++i) {
            winrt::com_ptr<IAudioSessionControl> session;
            DWORD pid = 0;
            if (FAILED(sessions->GetSession(i, session.put()))) continue;
            const auto owner = session.try_as<IAudioSessionControl2>();
            if (!owner || owner->GetProcessId(&pid) != S_OK || !pid) continue;
            if (inProcessTree(*processes, gameRoot, pid)) return GameOutput::Here;
        }
        return GameOutput::Elsewhere;
    } catch (...) {}
    return GameOutput::Unknown;
}

struct DiscordWatch::Guard {
    std::mutex mutex;
    DiscordWatch* owner = nullptr;
};

namespace {
// A new audio session on an output: an app started playing, perhaps Discord.
struct SessionNotifier : winrt::implements<SessionNotifier, IAudioSessionNotification> {
    std::function<void()> created;
    explicit SessionNotifier(std::function<void()> c) : created(std::move(c)) {}
    STDMETHODIMP OnSessionCreated(IAudioSessionControl*) noexcept override {
        created();
        return S_OK;
    }
};
} // namespace

struct DiscordWatch::Sessions {
    winrt::com_ptr<SessionNotifier> notifier;
    std::vector<winrt::com_ptr<IAudioSessionManager2>> managers;
};

DiscordWatch::DiscordWatch(std::function<void(DWORD)> changed)
    : changed_(std::move(changed)), guard_(std::make_shared<Guard>()), sessions_(std::make_unique<Sessions>()) {
    std::lock_guard lock(guard_->mutex);
    guard_->owner = this;
    // Discord may play voice on any output, so every active one reports new sessions.
    try {
        sessions_->notifier = winrt::make_self<SessionNotifier>([guard = guard_] {
            std::lock_guard lock(guard->mutex);
            if (guard->owner && !guard->owner->pid_) guard->owner->check();
        });
        const auto enumerator = winrt::create_instance<IMMDeviceEnumerator>(__uuidof(MMDeviceEnumerator));
        winrt::com_ptr<IMMDeviceCollection> outputs;
        winrt::check_hresult(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, outputs.put()));
        UINT count = 0;
        winrt::check_hresult(outputs->GetCount(&count));
        for (UINT i = 0; i < count; ++i) {
            winrt::com_ptr<IMMDevice> output;
            winrt::com_ptr<IAudioSessionManager2> manager;
            if (FAILED(outputs->Item(i, output.put())) ||
                FAILED(output->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, manager.put_void())) ||
                FAILED(manager->RegisterSessionNotification(sessions_->notifier.get()))) continue;
            // Windows reports new sessions only after the list has been asked for once.
            winrt::com_ptr<IAudioSessionEnumerator> existing;
            manager->GetSessionEnumerator(existing.put());
            sessions_->managers.push_back(std::move(manager));
        }
    } catch (...) {
        // Without notifications Discord is still found when replay starts.
    }
    check();
}

DiscordWatch::~DiscordWatch() {
    {
        std::lock_guard lock(guard_->mutex); // Waits for a running callback.
        guard_->owner = nullptr;
    }
    for (const auto& manager : sessions_->managers) manager->UnregisterSessionNotification(sessions_->notifier.get());
    sessions_.reset();
    if (wait_) UnregisterWaitEx(wait_, INVALID_HANDLE_VALUE);
    if (process_) CloseHandle(process_);
}

void CALLBACK DiscordWatch::exitedCallback(PVOID context, BOOLEAN) {
    auto* guard = static_cast<Guard*>(context);
    std::lock_guard lock(guard->mutex);
    if (guard->owner) guard->owner->exited();
}

// Runs under the guard's lock.
void DiscordWatch::exited() {
    // The one-shot wait has fired (this callback): release it without waiting for itself.
    UnregisterWaitEx(wait_, nullptr);
    CloseHandle(process_);
    wait_ = process_ = nullptr;
    pid_ = 0;
    if (changed_) changed_(pid_);
    check(); // Another Discord client may still be running.
}

// Runs under the guard's lock, while no Discord process is known.
void DiscordWatch::check() {
    const DWORD pid = findDiscordProcess();
    if (!pid) return;
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return;
    if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT) { CloseHandle(process); return; } // Already exiting.
    process_ = process;
    RegisterWaitForSingleObject(&wait_, process_, exitedCallback, guard_.get(), INFINITE, WT_EXECUTEONLYONCE);
    pid_ = pid;
    if (changed_) changed_(pid_);
}

} // namespace refarter::capture
