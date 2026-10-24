// update_check.h: version comparison, the minimal JSON reading and the daily rule. No network is used.
#include "update_check.h"
#include <iostream>

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    using namespace refarter::update;

    // ---- Versions ----
    check(isNewer(L"v0.5.2", L"0.5.1-alpha"), "0.5.2 is newer than 0.5.1-alpha");
    check(isNewer(L"0.5.1", L"0.5.1-alpha"), "the release beats its own pre-release");
    check(!isNewer(L"0.5.1-alpha", L"0.5.2"), "0.5.1-alpha is older than 0.5.2");
    check(!isNewer(L"v1.0.0-preview-beta", L"1.0.0"), "1.0.0-preview-beta is older than 1.0.0");
    check(isNewer(L"1.0.0", L"1.0.0-preview-beta"), "1.0.0 is newer than 1.0.0-preview-beta");
    check(!isNewer(L"v1.0.0", L"1.0.0"), "1.0.0 and v1.0.0 are the same version");
    check(!isNewer(L"1.0.0", L"v1.0.0"), "the same version is not newer, either way round");
    check(isNewer(L"0.10.0", L"0.9.9"), "numbers compare as numbers, not text");
    check(isNewer(L"1.0.0", L"0.99.99"), "the major number decides first");
    check(isNewer(L"1.2", L"1.1.9") && !isNewer(L"1.2", L"1.2.0"), "a missing patch number is 0");
    check(isNewer(L"V2.0.0", L"1.9.9"), "a capital V is accepted");
    check(!isNewer(L"0.5.0", L"0.5.1-alpha"), "an older release is not newer than a newer pre-release");
    check(isNewer(L"1.0.0-beta", L"1.0.0-alpha") && !isNewer(L"1.0.0-alpha", L"1.0.0-beta"), "alpha < beta");
    check(isNewer(L"1.0.0-rc.2", L"1.0.0-rc.1") && isNewer(L"1.0.0-rc.10", L"1.0.0-rc.9"), "numeric suffix parts compare as numbers");
    check(isNewer(L"1.0.0-rc.1", L"1.0.0-beta.9"), "rc is after beta");
    check(isNewer(L"1.0.0-alpha.1", L"1.0.0-alpha"), "a longer suffix with the same start is newer");
    check(!isNewer(L"1.0.0-ALPHA", L"1.0.0-alpha"), "suffixes ignore case");
    check(!isNewer(L"1.0.0+build7", L"1.0.0") && !isNewer(L"1.0.0", L"1.0.0+build7"), "build metadata is ignored");
    for (const wchar_t* garbage : {L"", L"v", L"latest", L"nightly-2026", L"1.2.3.4", L"1..2", L"1.a", L"-1.0.0", L"1.0.0-", L"1.0.0 beta",
                                    L"v1.0.0\n", L"9999999999.0.0", L"1.0.0-\u00e9", L"../../1.0.0"}) {
        check(!isNewer(garbage, L"0.0.1"), "garbage as the tag is never newer");
        check(!isNewer(L"99.0.0", garbage), "garbage as the running version means no claim");
        check(!parseVersion(garbage), "garbage does not parse");
    }
    check(parseVersion(L"v1.2.3-RC.1")->suffix == "rc.1" && parseVersion(L"1.2.3")->suffix.empty(), "suffixes are lower case and empty for a release");

    // ---- The daily rule ----
    const std::int64_t now = 1'800'000'000;
    check(due(0, now), "never checked is due");
    check(!due(now - 3600, now), "an hour ago is not due");
    check(!due(now - dayInSeconds, now), "exactly 24 hours is not yet due");
    check(due(now - dayInSeconds - 1, now), "more than 24 hours is due");
    check(due(now + 5000, now), "a time in the future (clock set back) is due");
    check(due(-3, now), "a negative time is due");

    // ---- The URLs come from one constant ----
    check(apiPath() == L"/repos/matonico/Refarter/releases/latest", "api path");
    check(fallbackPage() == L"https://github.com/matonico/Refarter/releases/latest", "fallback page");

    // ---- Saved notices still work when the daily rule skips a request ----
    const auto known = cachedRelease(L"v2.0.0", L"https://github.com/matonico/Refarter/releases/tag/v2.0.0", L"1.0.0");
    check(!due(now - 60, now) && known && known->tag == L"v2.0.0" &&
          known->url == L"https://github.com/matonico/Refarter/releases/tag/v2.0.0", "a recent check keeps its known update");
    check(!cachedRelease(L"v2.0.0", fallbackPage(), L"2.0.0") &&
          !cachedRelease(L"v2.0.0", fallbackPage(), L"3.0.0"), "installing that version or a newer one removes the notice");
    check(!cachedRelease(L"nightly", fallbackPage(), L"1.0.0") &&
          !cachedRelease(L"", fallbackPage(), L"1.0.0"), "invalid and missing saved versions do not show a notice");
    check(cachedRelease(L"v2.0.0", L"file:///bad", L"1.0.0")->url == fallbackPage(), "a saved unsafe link uses the safe fallback");

    // ---- Reading GitHub's answer ----
    // html_url and name also appear inside "author" and "assets"; only the top level counts.
    const std::string real =
        R"({"url":"https://api.github.com/repos/matonico/Refarter/releases/1","html_url":"https://github.com/matonico/Refarter/releases/tag/v1.0.0",)"
        R"("id":1,"author":{"login":"x","html_url":"https://github.com/x","tag_name":"nested"},"node_id":"A","tag_name":"v1.0.0","name":"1.0",)"
        R"("draft":false,"prerelease":false,"assets":[{"html_url":"https://example.com/evil","name":"a\"}]"}],"body":"line\nbreak \u00e9 {not json} \"tag_name\": \"v9\""})";
    const auto release = parseRelease(real);
    check(release && release->tag == L"v1.0.0" && release->url == L"https://github.com/matonico/Refarter/releases/tag/v1.0.0", "a real-looking answer");
    check(topLevelString(real, "tag_name") == "v1.0.0", "nested tag_name and text inside strings are not taken");
    check(topLevelString(real, "name") == "1.0", "another top-level key");
    check(!topLevelString(real, "login") && !topLevelString(real, "missing"), "nested and missing keys give nothing");
    check(!topLevelString(real, "draft") && !topLevelString(real, "id"), "a value that is not a string gives nothing");

    const auto spaced = parseRelease(" { \"tag_name\" :\n \"v2.0.0\" , \"html_url\" : \"https:\\/\\/github.com\\/matonico\\/Refarter\\/releases\\/tag\\/v2.0.0\" } ");
    check(spaced && spaced->tag == L"v2.0.0" && spaced->url == L"https://github.com/matonico/Refarter/releases/tag/v2.0.0", "whitespace and escaped slashes");
    check(topLevelString(R"({"a":"\u0041\u00e9\ud83d\ude00\\\"x"})", "a") == std::string("A\xc3\xa9\xf0\x9f\x98\x80\\\"x"), "unicode, surrogate pair and quote escapes");
    check(!topLevelString(R"({"a":"\ud83d"})", "a") && !topLevelString(R"({"a":"\ude00"})", "a") && !topLevelString(R"({"a":"\uZZZZ"})", "a") &&
          !topLevelString(R"({"a":"\q"})", "a") && !topLevelString(R"({"a":"\u12"})", "a"), "bad escapes are rejected");
    check(!topLevelString("{\"a\":\"line\nbreak\"}", "a"), "a raw control character in a string is rejected");

    const auto noUrl = parseRelease(R"({"tag_name":"v1.1.0"})");
    check(noUrl && noUrl->url == fallbackPage(), "no html_url uses the fallback page");
    for (const char* page : {R"x("javascript:alert(1)")x", R"("http://github.com/matonico/Refarter")", R"("https://github.com.evil.com/x")",
                             R"("https://evil.com/https://github.com/")", R"("https://github.com/a b")", R"("https://github.com/")", R"("")", R"(5)",
                             R"("file:///c:/windows/system32/calc.exe")", R"("https://github.com/x\u0000y")"}) {
        const auto odd = parseRelease(std::string(R"({"tag_name":"v1.1.0","html_url":)") + page + "}");
        check(odd && odd->url == fallbackPage(), "an unsafe html_url is replaced by the fallback page");
    }

    for (const char* bad : {"", "   ", "not json", "[]", "{}", "{\"tag_name\":", "{\"tag_name\":\"v1.0.0", "{\"tag_name\":5}", "{\"tag_name\":null}",
                            "{\"tag_name\":\"nightly\"}", "{\"tag_name\":\"\"}", "{\"message\":\"Not Found\",\"documentation_url\":\"x\"}",
                            "{\"tag_name\" \"v1.0.0\"}", "{\"tag_name\":\"v1.0.0\u00e9\"}", "<html>rate limited</html>", "}{", "{{{{{{{{", "{\"a\":[[[[[[[[[["}) {
        check(!parseRelease(bad), "bad or unrelated JSON gives no release");
    }
    // A hostile answer of any shape stays within bounds: deep nesting stops, huge strings are cut off by the limit.
    check(!parseRelease(std::string(100000, '[')) && !parseRelease(std::string("{\"x\":") + std::string(100, '[') + "}"), "deep nesting is refused");
    check(!parseRelease("{\"tag_name\":\"" + std::string(5000, '1') + "\"}"), "a tag longer than its limit is refused");
    check(parseRelease("{\"body\":\"" + std::string(200000, 'x') + "\",\"tag_name\":\"v3.0.0\"}").has_value(), "a long release body is skipped, not refused");
    check(parseRelease(R"({"tag_name":"v1.0.0","tag_name":"v9.9.9"})")->tag == L"v1.0.0", "the first tag_name wins");
    check(parseRelease(R"({"tag_name":"v1.0.0-rc.1"})")->tag == L"v1.0.0-rc.1", "a pre-release tag is still a version");

    check(displayVersion(L"v1.2.0") == L"1.2.0" && displayVersion(L"1.2.0") == L"1.2.0" && displayVersion(L"") == L"", "the display drops one leading v");

    return failures ? 1 : 0;
}
