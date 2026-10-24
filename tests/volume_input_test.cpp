#include "volume_input.h"
#include <iostream>

using namespace refarter;

int main() {
    const auto expect = [](const wchar_t* text, std::optional<int> want) {
        if (parseVolumePercent(text, 0, 200) != want) { std::wcerr << L"wrong result for \"" << text << L"\"\n"; return false; }
        return true;
    };
    bool ok = true;
    ok &= expect(L"110", 110);
    ok &= expect(L"110%", 110);
    ok &= expect(L" 85 % ", 85);
    ok &= expect(L"0", 0);
    ok &= expect(L"007", 7);
    ok &= expect(L"200", 200);
    ok &= expect(L"201", 200);                      // Clamped to the range.
    ok &= expect(L"99999999999999999999", 200);     // Too big for any integer: still the top.
    ok &= expect(L"", std::nullopt);
    ok &= expect(L"   ", std::nullopt);
    ok &= expect(L"%", std::nullopt);
    ok &= expect(L"%50", std::nullopt);
    ok &= expect(L"50%%", std::nullopt);
    ok &= expect(L"-5", std::nullopt);
    ok &= expect(L"+5", std::nullopt);
    ok &= expect(L"1 0", std::nullopt);
    ok &= expect(L"12a", std::nullopt);
    if (parseVolumePercent(L"5", 10, 100) != 10) { std::cerr << "low end not clamped\n"; ok = false; }
    return ok ? 0 : 1;
}
