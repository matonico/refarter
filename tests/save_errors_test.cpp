#include "save_errors.h"
#include <iostream>

int checkClipFolderPreflight();

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    using namespace refarter;
    check(isDiskFullCode(112), "ERROR_DISK_FULL");
    check(isDiskFullCode(39), "ERROR_HANDLE_DISK_FULL");
    check(isDiskFullCode(1295), "quota exceeded");
    check(isDiskFullCode(static_cast<long>(0x80070070)), "HRESULT_FROM_WIN32(ERROR_DISK_FULL)");
    check(isDiskFullCode(static_cast<long>(0x80070027)), "HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL)");
    check(!isDiskFullCode(0), "no error");
    check(!isDiskFullCode(5), "access denied");
    check(!isDiskFullCode(static_cast<long>(0x80004005)), "E_FAIL");
    check(!isDiskFullCode(static_cast<long>(0x80070005)), "E_ACCESSDENIED");
    check(!isDiskFullCode(static_cast<long>(0xC00D36B4)), "a Media Foundation code");
    check(isDiskFull(std::make_error_code(std::errc::no_space_on_device)), "no_space_on_device");
    check(isDiskFull(std::error_code(112, std::system_category())), "system 112");
    check(!isDiskFull(std::make_error_code(std::errc::permission_denied)), "permission denied");

    check(failedForDiskSpace(static_cast<long>(0x80004005), 0), "an unspecific failure with no free space is a full disk");
    check(failedForDiskSpace(static_cast<long>(0x80004005), lowDiskBytes - 1), "just under the limit");
    check(!failedForDiskSpace(static_cast<long>(0x80004005), lowDiskBytes), "plenty of space: not a disk problem");
    check(!failedForDiskSpace(static_cast<long>(0x80004005), ~0ull), "unknown free space: not blamed on the disk");
    check(failedForDiskSpace(112, ~0ull), "an explicit disk-full code needs no free-space reading");
    check(failedSaveText(diskFullMessage(), true).size() < 90, "message stays short");
    check(isDiskFullCode(static_cast<long>(0x80030070)), "STG_E_MEDIUMFULL");
    check(!isDiskFullCode(static_cast<long>(0x80030071)), "another storage error");
    check(failedForDiskSpace(static_cast<long>(0x80030070), ~0ull), "a storage medium-full code needs no free-space reading");
    check(failedSaveText(diskFullMessage(), true) == L"disk full · replay kept", "a kept replay is said to be kept");
    check(failedSaveText(diskFullMessage(), false) == diskFullMessage() && failedSaveText(diskFullMessage(), false).find(L"kept") == std::wstring::npos && failedSaveText(diskFullMessage(), false).find(L"save again") == std::wstring::npos,
          "a stop, quit or clear does not claim it is kept, nor suggest saving again");
    check(failedSaveText(L"could not write the clip", true) == L"could not write the clip", "other failures are shown as they are");
    for (const auto code : {112, 39, 1295}) {
        const auto message = folderFailureText(std::error_code(code, std::system_category()), L"folder unavailable");
        check(message == diskFullMessage(), "folder preflight preserves disk-full and quota errors");
        check(failedSaveText(message, true) == L"disk full · replay kept", "preflight failure keeps accurate replay note");
    }
    check(folderFailureText(std::error_code(5, std::system_category()), L"folder unavailable") == L"folder unavailable",
          "permission failure is not blamed on disk capacity");
    failures += checkClipFolderPreflight();
    return failures ? 1 : 0;
}
