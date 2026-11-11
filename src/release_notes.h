#pragma once

#include "update_check.h"

#include <string>
#include <string_view>
#include <vector>

namespace refarter::notes {

// The release notes of the running version (docs/releases/<version>.md, embedded at build time), read into the few
// kinds of block they use, for the "what's new" page. Pure, so the rules are unit-tested.

// A run of text in one style. Key is a <kbd> key, drawn in a small frame; Code is `text`, drawn in a fixed-width font.
struct Span {
    enum class Style { Plain, Bold, Code, Key };
    std::wstring text;
    Style style = Style::Plain;
    bool operator==(const Span&) const = default;
};

// Heading: "## …" or deeper. Bullet: "- …" (indented lines continue it). Paragraph: lines up to a blank one.
// Art: a fenced block, kept line for line (the kitten).
struct Block {
    enum class Kind { Heading, Bullet, Paragraph, Art };
    Kind kind = Kind::Paragraph;
    std::vector<Span> spans; // Empty for Art.
    std::wstring art;        // Art only: its lines, joined with '\n'.
    bool operator==(const Block&) const = default;
};

// **bold**, `code`, <kbd>key</kbd> and [text](link) (the link's text only: nothing on the page opens a link).
// An unmatched marker stays as it is written.
inline std::vector<Span> parseSpans(std::wstring_view text) {
    std::vector<Span> spans;
    std::wstring plain;
    const auto add = [&](std::wstring value, Span::Style style) {
        if (style == Span::Style::Plain) { plain += value; return; }
        if (!plain.empty()) spans.push_back({std::move(plain), Span::Style::Plain});
        plain.clear();
        if (!value.empty()) spans.push_back({std::move(value), style});
    };
    for (size_t at = 0; at < text.size();) {
        const auto rest = text.substr(at);
        if (rest.starts_with(L"**")) {
            if (const auto end = rest.find(L"**", 2); end != std::wstring_view::npos) {
                // Code and keys inside bold keep their own look; the rest of it is bold.
                for (auto inner : parseSpans(rest.substr(2, end - 2))) {
                    if (inner.style == Span::Style::Plain) inner.style = Span::Style::Bold;
                    add(std::move(inner.text), inner.style);
                }
                at += end + 2;
                continue;
            }
        } else if (rest.starts_with(L"`")) {
            if (const auto end = rest.find(L'`', 1); end != std::wstring_view::npos) {
                add(std::wstring(rest.substr(1, end - 1)), Span::Style::Code);
                at += end + 1;
                continue;
            }
        } else if (rest.starts_with(L"<kbd>")) {
            if (const auto end = rest.find(L"</kbd>"); end != std::wstring_view::npos) {
                add(std::wstring(rest.substr(5, end - 5)), Span::Style::Key);
                at += end + 6;
                continue;
            }
        } else if (rest.starts_with(L"[")) {
            const auto close = rest.find(L"](");
            const auto end = close == std::wstring_view::npos ? close : rest.find(L')', close);
            if (end != std::wstring_view::npos && rest.substr(1, close - 1).find(L'[') == std::wstring_view::npos) {
                add(std::wstring(rest.substr(1, close - 1)), Span::Style::Plain);
                at += end + 1;
                continue;
            }
        }
        plain += text[at++];
    }
    if (!plain.empty()) spans.push_back({std::move(plain), Span::Style::Plain});
    return spans;
}

// The whole notes file. Its "# Refarter <version>" title is left out (the page has its own), and so are the closing
// paragraphs straight after the last list: those say which file to download and link the changelog, which mean nothing
// inside the app. Paragraphs that follow anything else (the kitten, a heading) may be the notes themselves, so they stay.
inline std::vector<Block> parse(std::wstring_view markdown) {
    std::vector<Block> blocks;
    std::wstring paragraph;
    bool bullet = false, fence = false;
    std::wstring art;
    const auto flush = [&] {
        if (!paragraph.empty()) blocks.push_back({bullet ? Block::Kind::Bullet : Block::Kind::Paragraph, parseSpans(paragraph), {}});
        paragraph.clear();
        bullet = false;
    };
    for (size_t start = 0; start <= markdown.size();) {
        auto end = markdown.find(L'\n', start);
        if (end == std::wstring_view::npos) end = markdown.size();
        auto line = markdown.substr(start, end - start);
        start = end + 1;
        if (line.ends_with(L'\r')) line.remove_suffix(1);
        const auto first = line.find_first_not_of(L" \t");
        const auto trimmed = first == std::wstring_view::npos ? std::wstring_view{} : line.substr(first);
        if (trimmed.starts_with(L"```")) {
            if (fence) {
                if (!art.empty() && art.back() == L'\n') art.pop_back();
                blocks.push_back({Block::Kind::Art, {}, std::move(art)});
                art.clear();
            } else {
                flush();
            }
            fence = !fence;
            continue;
        }
        if (fence) { art.append(line); art += L'\n'; continue; }
        if (trimmed.empty()) { flush(); continue; }
        if (trimmed.starts_with(L"# ")) { flush(); continue; } // The file's title.
        if (trimmed.starts_with(L"##")) {
            flush();
            const auto marks = trimmed.find_first_not_of(L'#'); // npos: a line of '#' alone, an empty heading.
            const auto text = marks == std::wstring_view::npos ? std::wstring_view{} : trimmed.substr(marks);
            const auto from = text.find_first_not_of(L' ');
            if (from != std::wstring_view::npos) blocks.push_back({Block::Kind::Heading, parseSpans(text.substr(from)), {}});
            continue;
        }
        if (first == 0 && (trimmed.starts_with(L"- ") || trimmed.starts_with(L"* "))) {
            flush();
            bullet = true;
            paragraph = trimmed.substr(2);
            continue;
        }
        if (!paragraph.empty()) paragraph += L' ';
        paragraph += trimmed;
    }
    flush();
    if (fence && !art.empty()) blocks.push_back({Block::Kind::Art, {}, std::move(art)}); // An unclosed fence still shows.
    size_t keep = blocks.size();
    while (keep > 0 && blocks[keep - 1].kind == Block::Kind::Paragraph) --keep;
    if (keep > 0 && blocks[keep - 1].kind == Block::Kind::Bullet) blocks.resize(keep);
    return blocks;
}

// The notes as plain text, a block per line (bullets with "• "), for screen readers and a dialog that cannot show
// the formatted view.
inline std::wstring plainText(const std::vector<Block>& blocks) {
    std::wstring text;
    for (const auto& block : blocks) {
        if (!text.empty()) text += L'\n';
        if (block.kind == Block::Kind::Art) continue; // The kitten means nothing read aloud.
        if (block.kind == Block::Kind::Bullet) text += L"• ";
        for (const auto& span : block.spans) text += span.text;
    }
    return text;
}

// Whether this start should open the "what's new" page by itself. `seen` is the version whose notes were last shown
// (or skipped, on a fresh install); `fresh` means the settings file was created by this start, so nothing was updated.
// Only a step up shows the notes: going back to an older version, or a version that cannot be read, never does.
inline bool showOnStart(std::wstring_view seen, std::wstring_view running, bool fresh) {
    if (fresh) return false;
    if (seen.empty()) return true; // An update from before the page existed.
    return update::isNewer(running, seen);
}

} // namespace refarter::notes
