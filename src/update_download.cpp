#include "update_download.h"
#include "https_get.h"
#include "text.h"
#include "update_check.h"

#include <bcrypt.h>
#include <atomic>
#include <utility>
#include <vector>

namespace refarter {

namespace {
constexpr DWORD timeoutMs = 15'000; // Each step of a request; the body may take longer as a whole.

// SHA-256 with Windows' CNG, fed in pieces.
class Sha256 {
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) algorithm_ = nullptr;
        else if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) hash_ = nullptr;
    }
    ~Sha256() {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    bool add(const void* data, ULONG size) {
        return hash_ && BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)), size, 0) >= 0;
    }
    // Lower-case hex, or empty when hashing failed.
    std::string finish() {
        unsigned char digest[32]{};
        if (!hash_ || BCryptFinishHash(hash_, digest, sizeof(digest), 0) < 0) return {};
        static constexpr char digits[] = "0123456789abcdef";
        std::string text;
        for (const unsigned char byte : digest) { text += digits[byte >> 4]; text += digits[byte & 15]; }
        return text;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

// Unreadable: the file is missing, held by another program, or could not be read or hashed; says nothing of its content.
enum class FileCheck { Matches, Differs, Unreadable };

// Opens the file shared for reading only (nothing may write, rename or delete it while the handle is open) and
// hashes it through that handle. With `lock`, a file that matches stays open through it.
FileCheck checkFile(const std::filesystem::path& path, const std::string& expected, std::shared_ptr<void>* lock = nullptr) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return FileCheck::Unreadable;
    std::shared_ptr<void> handle(file, CloseHandle);
    Sha256 sha;
    std::vector<char> chunk(64 * 1024);
    DWORD read = 0;
    while (ReadFile(file, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr)) {
        if (read != 0) {
            if (!sha.add(chunk.data(), read)) return FileCheck::Unreadable;
            continue;
        }
        const auto hash = sha.finish();
        if (hash.empty()) return FileCheck::Unreadable;
        if (hash != expected) return FileCheck::Differs;
        if (lock) *lock = std::move(handle);
        return FileCheck::Matches;
    }
    return FileCheck::Unreadable;
}

// Everything in the folder except `keep`: older installers and broken downloads. Errors (a file another worker still
// has open) are left for the next time. Runs on the worker, where a throwing iterator would end the whole process.
void clearFolder(const std::filesystem::path& folder, const std::filesystem::path& keep) {
    std::error_code error;
    std::filesystem::directory_iterator entry(folder, error);
    const std::filesystem::directory_iterator end;
    for (; !error && entry != end; entry.increment(error)) {
        std::error_code ignored;
        if (entry->path() != keep && entry->is_regular_file(ignored)) std::filesystem::remove(entry->path(), ignored);
    }
}

std::atomic<LPARAM> lastId{0};
} // namespace

void UpdateDownload::start(HWND window, UINT done, UINT progress, std::wstring tag, std::wstring userAgent, std::filesystem::path folder) {
    cancel();
    id_ = ++lastId;
    auto run = [window, progress, id = id_, tag = std::move(tag), userAgent = std::move(userAgent),
                folder = std::move(folder)](const auto& worker) {
        // Never competes with the game: background CPU, disk and memory priority for the whole download.
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        DownloadResult result;
        result.tag = tag;
        using Outcome = DownloadResult::Outcome;
        const auto name = update::setupName(tag);
        const auto sumsPath = update::assetPath(tag, L"SHA256SUMS.txt"), setupPath = update::assetPath(tag, name);
        // A partial file of this download's own: a cancelled worker may still be writing its own for a few seconds.
        const std::filesystem::path target = folder / name, partial = folder / (name + L"." + std::to_wstring(id) + L".partial");
        std::optional<std::string> expected;
        if (sumsPath && setupPath && !folder.empty()) {
            std::string sums;
            const auto answer = httpsGet(userAgent, update::downloadHost, *sumsPath, nullptr, timeoutMs,
                [&](const char* data, DWORD size, std::uint64_t) {
                    if (sums.size() + size > update::maxSumsBytes || worker.cancelled()) return false;
                    sums.append(data, size);
                    return true;
                });
            if (answer.complete && answer.status == 200) expected = update::expectedHash(sums, utf8(name));
            // Nothing to check against, so retrying would not help: the release has no SHA256SUMS.txt (404), or its
            // list does not name this setup. Any other answer (503, a rate limit, offline) stays a network failure.
            if (answer.complete && (answer.status == 404 || (answer.status == 200 && !expected))) result.outcome = Outcome::NoChecksum;
        }
        // Cancelled during the request: leave the folder to the download that may have started since.
        if (worker.cancelled()) expected.reset();
        std::error_code error;
        if (expected) {
            std::filesystem::create_directories(folder, error); // False with no error when it exists already.
            if (error) { result.outcome = Outcome::Disk; expected.reset(); }
        }
        if (expected) {
            clearFolder(folder, target);
            std::shared_ptr<void> lock;
            // Downloaded before and still whole. One that differs goes now, so a failed download cannot leave it behind.
            if (checkFile(target, *expected, &lock) == FileCheck::Differs) std::filesystem::remove(target, error);
            if (!lock && !worker.cancelled()) {
                const HANDLE file = CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file == INVALID_HANDLE_VALUE) {
                    result.outcome = Outcome::Disk;
                } else {
                    std::uint64_t received = 0;
                    int percent = -1;
                    bool written = true;
                    const auto answer = httpsGet(userAgent, update::downloadHost, *setupPath, nullptr, timeoutMs,
                        [&](const char* data, DWORD size, std::uint64_t length) {
                            if (worker.cancelled() || received + size > update::maxSetupBytes) return false;
                            DWORD wrote = 0;
                            if (!WriteFile(file, data, size, &wrote, nullptr) || wrote != size) { written = false; return false; }
                            received += size;
                            if (const int now = length ? static_cast<int>(received * 100 / length) : -1; now != percent && now >= 0 && now <= 100) {
                                percent = now;
                                PostMessageW(window, progress, static_cast<WPARAM>(percent), id);
                            }
                            return true;
                        });
                    CloseHandle(file);
                    if (!written) result.outcome = Outcome::Disk;
                    else if (!answer.complete || answer.status != 200) result.outcome = Outcome::Network;
                    else if (const auto downloaded = checkFile(partial, *expected); downloaded != FileCheck::Matches) {
                        // Checked before it gets the setup's name: a bad download never lies there as a runnable file.
                        result.outcome = downloaded == FileCheck::Differs ? Outcome::Mismatch : Outcome::Disk;
                    } else if (!MoveFileExW(partial.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                        result.outcome = Outcome::Disk;
                    } else if (const auto moved = checkFile(target, *expected, &lock); moved != FileCheck::Matches) {
                        // Changed between the two checks; only this one holds the file until setup runs.
                        result.outcome = moved == FileCheck::Differs ? Outcome::Mismatch : Outcome::Disk;
                        std::filesystem::remove(target, error);
                    }
                    std::filesystem::remove(partial, error); // Already gone after the rename.
                }
            }
            if (lock) {
                result.outcome = Outcome::Ready;
                result.setup = target;
                result.lock = std::move(lock);
            }
        }
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        return result; // Dropped unposted when cancelled.
    };
    // No thread: nothing downloads; running() stays false and the caller reports it.
    task_.start(window, done, std::move(run));
}

} // namespace refarter
