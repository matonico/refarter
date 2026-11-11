#pragma once

#include "release_notes.h"
#include "theme.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <winrt/base.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace refarter {

// The release notes built into this exe (docs/releases/<version>.md at build time), read once; empty when the build
// had none.
const std::vector<notes::Block>& embeddedNotes();

// The body of the "what's new" popup and of the codec help ("which codec should i pick?"): headings, bullets,
// paragraphs and the release's ASCII kitten, wrapped to the width and scrolled as one column. A child window that scrolls with the mouse wheel and the scrolling keys (its dialog
// passes them on); nothing in it is clickable. Text is drawn with DirectWrite so the notes' emoji show in colour, as
// GDI would draw them only as outlines. It lays out only when its size, DPI or notes change.
class NotesView {
public:
    NotesView() = default;
    NotesView(const NotesView&) = delete;
    NotesView& operator=(const NotesView&) = delete;

    // nullptr also when text could not be drawn here: the caller shows plain text. `name` is what screen readers call it.
    HWND create(HWND parent, int id, const wchar_t* name = L"what's new");
    int contentHeight() const { return content_; } // Everything laid out at the current width, device pixels.
    void setBackground(COLORREF colour) { background_ = colour; }
    bool setStyle(UINT dpi); // Makes the text formats at this DPI and lays out again; false when one could not be made.
    void setNotes(std::vector<notes::Block> blocks);

private:
    enum class Face { Body, Bold, Heading, Code, Art, count };
    // A word (with the spaces after it) or a key, placed on its line. Device pixels from the column's top left.
    struct Piece {
        std::wstring text;
        Face face = Face::Body;
        bool key = false; // Drawn in a small frame.
        COLORREF colour = theme::ink;
        int x = 0, y = 0;
        int width = 0;  // The word without its trailing spaces, or the key's frame.
        int height = 0; // Its line.
    };
    struct Dot { int x, y; }; // A bullet's mark, centred on its first line.

    HWND window_ = nullptr;
    UINT dpi_ = 96;
    COLORREF background_ = theme::background;
    winrt::com_ptr<IDWriteFactory> writer_;
    winrt::com_ptr<ID2D1Factory> d2d_;
    winrt::com_ptr<ID2D1DCRenderTarget> target_; // Draws the text into the back buffer's DC.
    winrt::com_ptr<IDWriteTextFormat> formats_[static_cast<int>(Face::count)];
    std::vector<notes::Block> blocks_;
    std::vector<Piece> pieces_;
    std::vector<Dot> dots_;
    std::unordered_map<std::wstring, std::pair<int, int>> widths_[static_cast<int>(Face::count)]; // measure()'s answers, by face.
    int content_ = 0; // Height of everything laid out.
    bool gutter_ = false; // The last layout kept room for the scroll indicator (the notes did not fit).
    int scroll_ = 0;
    int width_ = 0, height_ = 0;
    theme::BackBuffer buffer_;

    int px(int value) const { return MulDiv(value, static_cast<int>(dpi_), 96); }
    IDWriteTextFormat* formatOf(Face face) const { return formats_[static_cast<int>(face)].get(); }
    // Width of `text` in device pixels: without trailing spaces (the line-break test) and with them (the advance).
    std::pair<int, int> measure(std::wstring_view text, Face face);
    void layout();
    void layoutAt(int right);
    void scrollTo(int position);
    void paint(HDC dc);
    bool makeTarget();
    bool canDraw();
    void drawText(HDC dc);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
};

} // namespace refarter
