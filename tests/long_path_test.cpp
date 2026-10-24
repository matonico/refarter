#include "long_path.h"

#include <iostream>

using namespace refarter;

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    const std::wstring tail(300, L'a');
    const std::wstring shortDrive = L"C:\\clips\\a.mp4";
    check(extendedPathText(shortDrive) == shortDrive, "short drive paths are unchanged");
    check(extendedPathText(L"\\\\server\\share\\a.mp4") == L"\\\\server\\share\\a.mp4", "short network paths are unchanged");
    check(extendedPathText(L"C:\\clips\\" + tail) == L"\\\\?\\C:\\clips\\" + tail, "long drive paths get the extended prefix");
    check(extendedPathText(L"\\\\server\\share\\" + tail) == L"\\\\?\\UNC\\server\\share\\" + tail, "long network paths get the UNC prefix");
    check(extendedPathText(L"C:/clips/" + tail) == L"\\\\?\\C:\\clips\\" + tail, "long paths with slashes are converted");
    check(extendedPathText(L"\\\\?\\C:\\clips\\" + tail) == L"\\\\?\\C:\\clips\\" + tail, "already prefixed paths are unchanged");
    // The limit is 260 characters: 259 stays, 260 is prefixed.
    const std::wstring at259 = L"C:\\" + std::wstring(256, L'a');
    check(at259.size() == 259 && extendedPathText(at259) == at259, "259 characters stay");
    const std::wstring at260 = at259 + L"a";
    check(extendedPathText(at260) == L"\\\\?\\" + at260, "260 characters are prefixed");
    return failures ? 1 : 0;
}
