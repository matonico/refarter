#pragma once

// The pure half of "is a newer version available": which release to ask about, how a tag compares with the
// running version, how little of GitHub's answer is read, and when a check is due. No network and no Windows here;
// update_client.cpp does the request on a worker thread and tests/update_check_test.cpp covers all of this.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace refarter::update {

// The one place that names the public repository ("owner/repo").
inline constexpr std::wstring_view repository = L"matonico/Refarter";
inline constexpr std::wstring_view apiHost = L"api.github.com";
inline constexpr std::wstring_view releasePrefix = L"https://github.com/";

inline std::wstring apiPath() { return L"/repos/" + std::wstring(repository) + L"/releases/latest"; }
// Where "update available" leads when the answer carries no usable html_url.
inline std::wstring fallbackPage() { return std::wstring(releasePrefix) + std::wstring(repository) + L"/releases/latest"; }

inline constexpr std::int64_t dayInSeconds = 24 * 60 * 60;
inline constexpr std::size_t maxResponseBytes = 256 * 1024;

// A check at start-up is due when none got an answer in the last 24 hours. A stored time in the future (the clock
// was set back) or no stored time is due, so a bad value cannot stop checks for good.
inline bool due(std::int64_t lastCheck, std::int64_t now) {
    return lastCheck <= 0 || lastCheck > now || now - lastCheck > dayInSeconds;
}

struct Version {
    long major = 0, minor = 0, patch = 0;
    std::string suffix; // After the first '-', lower case; empty for a release.
};

// "v1.2.3", "1.2", "0.5.1-alpha" or "1.0.0-rc.1+build5": up to three numbers, an optional leading v and an optional
// pre-release suffix. Anything else (a fourth number, letters in a number, an empty suffix) is not a version.
inline std::optional<Version> parseVersion(std::wstring_view text) {
    if (!text.empty() && (text.front() == L'v' || text.front() == L'V')) text.remove_prefix(1);
    if (const auto plus = text.find(L'+'); plus != std::wstring_view::npos) text = text.substr(0, plus);
    if (text.empty() || text.size() > 64) return std::nullopt;
    Version version;
    long* parts[] = {&version.major, &version.minor, &version.patch};
    size_t position = 0;
    int count = 0;
    while (true) {
        size_t digits = 0;
        long value = 0;
        while (position < text.size() && text[position] >= L'0' && text[position] <= L'9') {
            if (++digits > 9) return std::nullopt;
            value = value * 10 + (text[position] - L'0');
            ++position;
        }
        if (!digits || count == 3) return std::nullopt;
        *parts[count++] = value;
        if (position == text.size()) return version;
        if (text[position] == L'.') { ++position; continue; }
        if (text[position] == L'-') break;
        return std::nullopt;
    }
    ++position; // The '-'.
    if (position >= text.size()) return std::nullopt;
    for (; position < text.size(); ++position) {
        const wchar_t c = text[position];
        if (c > 0x7e || c <= 0x20) return std::nullopt;
        version.suffix += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return version;
}

namespace detail {
inline std::vector<std::string> identifiers(const std::string& suffix) {
    std::vector<std::string> result(1);
    for (const char c : suffix) {
        if (c == '.' || c == '-') result.emplace_back();
        else result.back() += c;
    }
    return result;
}
inline bool numeric(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}
// Negative: a before b. Numbers compare as numbers and come before words; words by their letters.
inline int compareIdentifier(const std::string& a, const std::string& b) {
    const bool x = numeric(a), y = numeric(b);
    if (x && y) {
        const auto trim = [](const std::string& s) { const auto at = s.find_first_not_of('0'); return at == std::string::npos ? std::string("0") : s.substr(at); };
        const auto left = trim(a), right = trim(b);
        if (left.size() != right.size()) return left.size() < right.size() ? -1 : 1;
        return left.compare(right) < 0 ? -1 : left == right ? 0 : 1;
    }
    if (x != y) return x ? -1 : 1;
    return a.compare(b) < 0 ? -1 : a == b ? 0 : 1;
}
} // namespace detail

// Negative: a is older than b. The numbers decide first; with the same numbers a release is newer than any
// pre-release, and two pre-releases compare identifier by identifier ("alpha" < "beta" < "rc.1" < "rc.2").
inline int compareVersions(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.suffix.empty() != b.suffix.empty()) return a.suffix.empty() ? 1 : -1;
    const auto x = detail::identifiers(a.suffix), y = detail::identifiers(b.suffix);
    for (size_t i = 0; i < x.size() && i < y.size(); ++i)
        if (const int order = detail::compareIdentifier(x[i], y[i])) return order;
    return x.size() == y.size() ? 0 : x.size() < y.size() ? -1 : 1;
}

// Whether `tag` (as a release names it) is newer than `running` ("0.5.1-alpha"). Either one not being a version
// means no: an update is never claimed on a guess.
inline bool isNewer(std::wstring_view tag, std::wstring_view running) {
    const auto latest = parseVersion(tag), current = parseVersion(running);
    return latest && current && compareVersions(*latest, *current) > 0;
}

// ---- The answer ---------------------------------------------------------------------------------------------------

namespace detail {
inline void appendUtf8(std::string& out, unsigned long code) {
    if (code < 0x80) out += static_cast<char>(code);
    else if (code < 0x800) { out += static_cast<char>(0xc0 | (code >> 6)); out += static_cast<char>(0x80 | (code & 0x3f)); }
    else if (code < 0x10000) {
        out += static_cast<char>(0xe0 | (code >> 12)); out += static_cast<char>(0x80 | ((code >> 6) & 0x3f)); out += static_cast<char>(0x80 | (code & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (code >> 18)); out += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3f)); out += static_cast<char>(0x80 | (code & 0x3f));
    }
}
inline std::optional<unsigned long> hex4(std::string_view json, size_t at) {
    if (at + 4 > json.size()) return std::nullopt;
    unsigned long value = 0;
    for (size_t i = 0; i < 4; ++i) {
        const char c = json[at + i];
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return std::nullopt;
        value = value * 16 + static_cast<unsigned long>(digit);
    }
    return value;
}
// The string whose opening quote is at json[at]; `end` receives the index after its closing quote. Longer than
// `limit` bytes, unterminated or malformed gives nothing.
inline std::optional<std::string> readString(std::string_view json, size_t at, size_t& end, size_t limit) {
    std::string out;
    for (size_t i = at + 1; i < json.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(json[i]);
        if (c == '"') { end = i + 1; return out; }
        if (c < 0x20) return std::nullopt; // JSON forbids raw control characters in strings.
        if (c != '\\') out += static_cast<char>(c);
        else {
            if (++i >= json.size()) return std::nullopt;
            switch (json[i]) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                auto code = hex4(json, i + 1);
                if (!code) return std::nullopt;
                i += 4;
                if (*code >= 0xd800 && *code <= 0xdbff) { // A high surrogate needs its low half.
                    if (i + 6 >= json.size() || json[i + 1] != '\\' || json[i + 2] != 'u') return std::nullopt;
                    const auto low = hex4(json, i + 3);
                    if (!low || *low < 0xdc00 || *low > 0xdfff) return std::nullopt;
                    code = 0x10000 + ((*code - 0xd800) << 10) + (*low - 0xdc00);
                    i += 6;
                } else if (*code >= 0xdc00 && *code <= 0xdfff) return std::nullopt;
                appendUtf8(out, *code);
                break;
            }
            default: return std::nullopt;
            }
        }
        if (out.size() > limit) return std::nullopt;
    }
    return std::nullopt;
}
} // namespace detail

// The string value of `key` among the top-level members of the JSON object `json`. Nested objects and arrays are
// skipped (GitHub's answer repeats html_url inside "author" and "assets"), strings are read whole so brackets inside
// them mean nothing, and a value that is not a string gives nothing. This is not a validator.
inline std::optional<std::string> topLevelString(std::string_view json, std::string_view key, size_t limit = 1024) {
    int depth = 0;
    bool expectKey = false;
    for (size_t i = 0; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '{' || c == '[') {
            if (++depth > 64) return std::nullopt;
            expectKey = depth == 1 && c == '{';
        } else if (c == '}' || c == ']') {
            if (--depth < 0) return std::nullopt;
        } else if (c == ',') {
            expectKey = depth == 1;
        } else if (c == '"') {
            size_t end = 0;
            const auto text = detail::readString(json, i, end, depth == 1 && expectKey ? 256 : json.size()); // Release notes are long; they are read and dropped.
            if (!text) return std::nullopt;
            i = end - 1;
            if (depth != 1 || !expectKey) continue;
            expectKey = false;
            size_t next = end;
            while (next < json.size() && std::isspace(static_cast<unsigned char>(json[next]))) ++next;
            if (next >= json.size() || json[next] != ':') return std::nullopt;
            ++next;
            while (next < json.size() && std::isspace(static_cast<unsigned char>(json[next]))) ++next;
            if (*text != key) { i = next - 1; continue; } // The value is scanned by the loop (strings whole, brackets counted).
            if (next >= json.size() || json[next] != '"') return std::nullopt;
            return detail::readString(json, next, end, limit);
        }
    }
    return std::nullopt;
}

struct Release {
    std::wstring tag; // As published ("v1.0.0"); a version.
    std::wstring url; // The release page, https on github.com.
};

namespace detail {
inline std::optional<std::wstring> asciiWide(const std::string& text) {
    std::wstring wide;
    for (const unsigned char c : text) {
        if (c <= 0x20 || c > 0x7e) return std::nullopt;
        wide += static_cast<wchar_t>(c);
    }
    return wide;
}
} // namespace detail

// A release page worth opening: https on github.com, plain printable characters, nothing else.
inline bool safeReleaseUrl(std::wstring_view url) {
    return url.size() <= 512 && url.substr(0, releasePrefix.size()) == releasePrefix && url.size() > releasePrefix.size() &&
           std::none_of(url.begin(), url.end(), [](wchar_t c) { return c <= 0x20 || c > 0x7e || c == L'"' || c == L'<' || c == L'>' || c == L'^' || c == L'`'; });
}

// tag_name and html_url from GitHub's answer. Nothing without a tag that is a version; an unusable html_url is
// replaced by the repository's latest-release page.
inline std::optional<Release> parseRelease(std::string_view json) {
    const auto tag = topLevelString(json, "tag_name", 128);
    const auto wideTag = tag ? detail::asciiWide(*tag) : std::nullopt;
    if (!wideTag || !parseVersion(*wideTag)) return std::nullopt;
    Release release{*wideTag, fallbackPage()};
    if (const auto page = topLevelString(json, "html_url", 512)) {
        if (const auto wide = detail::asciiWide(*page); wide && safeReleaseUrl(*wide)) release.url = *wide;
    }
    return release;
}

// Restore a known update without another request. An installed upgrade or an invalid tag removes the notice;
// a hand-edited link follows the same safety rule as a network answer.
inline std::optional<Release> cachedRelease(std::wstring_view tag, std::wstring_view url, std::wstring_view running) {
    if (tag.size() > 128 || !isNewer(tag, running)) return std::nullopt;
    return Release{std::wstring(tag), safeReleaseUrl(url) ? std::wstring(url) : fallbackPage()};
}

// "1.0.0" for "v1.0.0": what the interface shows after its own "v".
inline std::wstring displayVersion(std::wstring_view tag) {
    if (!tag.empty() && (tag.front() == L'v' || tag.front() == L'V')) tag.remove_prefix(1);
    return std::wstring(tag);
}

} // namespace refarter::update
