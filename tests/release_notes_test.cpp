// release_notes.h: the release notes read into blocks for the "what's new" page, and when the page opens by itself.
#include "release_notes.h"
#include <iostream>

int main() {
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    using namespace refarter::notes;
    using Style = Span::Style;
    using Kind = Block::Kind;

    // ---- Inline styles ----
    check(parseSpans(L"plain text") == std::vector<Span>{{L"plain text", Style::Plain}}, "plain text is one span");
    check(parseSpans(L"a **bold** b") == std::vector<Span>{{L"a ", Style::Plain}, {L"bold", Style::Bold}, {L" b", Style::Plain}},
        "bold between plain text");
    check(parseSpans(L"press <kbd>Ctrl</kbd>+<kbd>F</kbd>") ==
        std::vector<Span>{{L"press ", Style::Plain}, {L"Ctrl", Style::Key}, {L"+", Style::Plain}, {L"F", Style::Key}}, "keys");
    check(parseSpans(L"unzip `Refarter.zip` now") ==
        std::vector<Span>{{L"unzip ", Style::Plain}, {L"Refarter.zip", Style::Code}, {L" now", Style::Plain}}, "code");
    check(parseSpans(L"see [the changelog](https://example.com) here") == std::vector<Span>{{L"see the changelog here", Style::Plain}},
        "a link keeps only its text");
    check(parseSpans(L"**Zoom with <kbd>+</kbd>.** Then") ==
        std::vector<Span>{{L"Zoom with ", Style::Bold}, {L"+", Style::Key}, {L".", Style::Bold}, {L" Then", Style::Plain}},
        "a key inside bold keeps its frame");
    check(parseSpans(L"2 ** 3 and a ` tick") == std::vector<Span>{{L"2 ** 3 and a ` tick", Style::Plain}}, "unmatched markers stay as text");
    check(parseSpans(L"[x] and [y](z)") == std::vector<Span>{{L"[x] and y", Style::Plain}}, "brackets that are not a link stay");
    check(parseSpans(L"****") == std::vector<Span>{}, "empty bold adds nothing");

    // ---- Blocks ----
    const auto blocks = parse(
        L"# Refarter 1.1.0\r\n"
        L"\r\n"
        L"```text\r\n"
        L"  /\\_/\\\r\n"
        L" ( o.o )\r\n"
        L"```\r\n"
        L"\r\n"
        L"Changes since 1.0.0:\r\n"
        L"\r\n"
        L"## Built-in player\r\n"
        L"\r\n"
        L"- 🔍 **Zoom.** The wheel zooms.\r\n"
        L"- 🐢 **Speed.** Plays slower\r\n"
        L"  or faster.\r\n"
        L"\r\n"
        L"### Smaller\r\n"
        L"* Fixes.\r\n"
        L"\r\n"
        L"Install `Refarter-1.1.0-setup.exe`, or unzip\r\n"
        L"the portable zip.\r\n"
        L"\r\n"
        L"[Full changelog](https://github.com/x/y/compare/v1.0.0...v1.1.0)\r\n");
    check(blocks.size() == 7, "title and closing paragraphs are left out");
    if (blocks.size() == 7) {
        check(blocks[0].kind == Kind::Art && blocks[0].art == L"  /\\_/\\\n ( o.o )", "the fenced kitten is kept line for line");
        check(blocks[1].kind == Kind::Paragraph && blocks[1].spans == std::vector<Span>{{L"Changes since 1.0.0:", Style::Plain}},
            "a paragraph before the lists stays");
        check(blocks[2].kind == Kind::Heading && blocks[2].spans == std::vector<Span>{{L"Built-in player", Style::Plain}}, "## heading");
        check(blocks[3].kind == Kind::Bullet && blocks[3].spans.size() == 3 && blocks[3].spans[1] == Span{L"Zoom.", Style::Bold},
            "a bullet with bold");
        check(blocks[4].kind == Kind::Bullet && blocks[4].spans.back() == Span{L" Plays slower or faster.", Style::Plain},
            "an indented line continues its bullet");
        check(blocks[5].kind == Kind::Heading && blocks[5].spans.front().text == L"Smaller", "### heading");
        check(blocks[6].kind == Kind::Bullet && blocks[6].spans.front().text == L"Fixes.", "a * bullet");
    }
    check(parse(L"").empty(), "no notes, no blocks");
    check(parse(L"Only a paragraph.\n\nAnd another.").size() == 2, "notes without a list keep their paragraphs");
    check(parse(L"```\nnever closed\n").size() == 1, "an unclosed fence still shows");
    check(parse(L"- one\n- two\n\nclosing").size() == 2, "the closing paragraph after the last list goes");
    check(parse(L"```\n(o.o)\n```\nFixes a crash.\n\nInstall it.").size() == 3, "notes with a kitten and no list keep their text");
    check(parse(L"## Fixes\nA crash is gone.\n\nInstall it.").size() == 3, "a paragraph under a heading stays: the heading is not left empty");
    check(parse(L"- one\n\n## Fixes\nA crash is gone.").size() == 3, "a paragraph under a later heading stays");
    check(parse(L"##\n###   \n- one").size() == 1, "a heading with no text is skipped, not a crash");
    check(plainText(parse(L"```\n(o.o)\n```\nIntro.\n\n## Player\n- **Zoom.** Wheel <kbd>+</kbd>.\n- Speed.")) ==
        L"Intro.\nPlayer\n• Zoom. Wheel +.\n• Speed.", "plain text for screen readers leaves out the art and the markup");

    // ---- When the page opens by itself ----
    check(!showOnStart(L"", L"1.1.0", true), "a fresh install shows nothing");
    check(showOnStart(L"", L"1.1.0", false), "an update from before the page existed shows the notes");
    check(showOnStart(L"1.0.0", L"1.1.0", false), "an update shows the notes");
    check(showOnStart(L"1.1.0-beta", L"1.1.0", false), "a release after its own pre-release shows the notes");
    check(!showOnStart(L"1.1.0", L"1.1.0", false), "the same version shows them only once");
    check(!showOnStart(L"1.2.0", L"1.1.0", false), "going back to an older version shows nothing");
    check(!showOnStart(L"garbage", L"1.1.0", false), "an unreadable stored version shows nothing");

    if (failures) std::cerr << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
