#include "notes_view.h"
#include "resources.h"

#include <algorithm>
#include <cmath>

namespace refarter {
namespace {
constexpr wchar_t className[] = L"RefarterNotes";
constexpr int indicatorRoom = 16; // Kept free at the right for the scroll indicator (logical px).
constexpr int bulletIndent = 20, keyPadding = 6, keyGap = 2;

std::wstring decodeUtf8(std::string_view bytes) {
    if (bytes.starts_with("\xEF\xBB\xBF")) bytes.remove_prefix(3);
    if (bytes.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring text(static_cast<size_t>(std::max(size, 0)), L'\0');
    if (size > 0) MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), size);
    return text;
}

D2D1_COLOR_F colourOf(COLORREF value) {
    return {GetRValue(value) / 255.f, GetGValue(value) / 255.f, GetBValue(value) / 255.f, 1.f};
}
} // namespace

const std::vector<notes::Block>& embeddedNotes() {
    static const std::vector<notes::Block> blocks = [] {
        const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_RELEASE_NOTES), RT_RCDATA);
        const auto loaded = resource ? LoadResource(nullptr, resource) : nullptr;
        if (!loaded) return std::vector<notes::Block>{};
        return notes::parse(decodeUtf8({static_cast<const char*>(LockResource(loaded)), SizeofResource(nullptr, resource)}));
    }();
    return blocks;
}

HWND NotesView::create(HWND parent, int id, const wchar_t* name) {
    if (!theme::registerWindowClass(className, proc)) return nullptr;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(writer_.put()))) ||
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.put()))) return nullptr;
    window_ = CreateWindowExW(0, className, name, WS_CHILD, 0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), this);
    if (window_ && !canDraw()) DestroyWindow(window_); // Clears window_.
    return window_;
}

bool NotesView::makeTarget() {
    if (target_) return true;
    const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.f, 96.f);
    return d2d_ && SUCCEEDED(d2d_->CreateDCRenderTarget(&properties, target_.put()));
}

// A pass over a small buffer made as WM_PAINT makes its own, while the dialog can still choose plain text: a failure
// at the first paint would leave the body blank, and the popup does not open by itself twice.
bool NotesView::canDraw() {
    const HDC screen = GetDC(window_);
    theme::BackBuffer probe;
    const HDC memory = screen ? probe.acquire(screen, 1, 1) : nullptr;
    const RECT bounds{0, 0, 1, 1};
    bool drawn = false;
    if (memory && makeTarget() && SUCCEEDED(target_->BindDC(memory, &bounds))) {
        target_->BeginDraw();
        drawn = SUCCEEDED(target_->EndDraw());
    }
    if (screen) ReleaseDC(window_, screen);
    return drawn;
}

// Sizes are device pixels: the render target works at 96 dpi, so one DIP is one pixel.
bool NotesView::setStyle(UINT dpi) {
    dpi_ = dpi;
    for (auto& known : widths_) known.clear(); // Measured at the old size.
    const auto make = [&](Face face, const wchar_t* family, DWRITE_FONT_WEIGHT weight, int size) {
        auto& format = formats_[static_cast<int>(face)];
        format = nullptr;
        if (!writer_ || FAILED(writer_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(px(size)), L"en-gb", format.put()))) return;
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    };
    make(Face::Body, L"Segoe UI", DWRITE_FONT_WEIGHT_NORMAL, 14);
    make(Face::Bold, L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 14);
    make(Face::Heading, L"Segoe UI", DWRITE_FONT_WEIGHT_SEMI_BOLD, 17);
    make(Face::Code, L"Consolas", DWRITE_FONT_WEIGHT_NORMAL, 13);
    make(Face::Art, L"Consolas", DWRITE_FONT_WEIGHT_NORMAL, 14);
    layout();
    return std::all_of(std::begin(formats_), std::end(formats_), [](const auto& format) { return format != nullptr; });
}

void NotesView::setNotes(std::vector<notes::Block> blocks) {
    blocks_ = std::move(blocks);
    scroll_ = 0;
    layout();
}

// Kept per word and face until the DPI changes, so a resize lays out again without asking DirectWrite.
std::pair<int, int> NotesView::measure(std::wstring_view text, Face face) {
    auto& known = widths_[static_cast<int>(face)];
    if (const auto found = known.find(std::wstring(text)); found != known.end()) return found->second;
    winrt::com_ptr<IDWriteTextLayout> line;
    DWRITE_TEXT_METRICS metrics{};
    if (!formatOf(face) || FAILED(writer_->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), formatOf(face),
        1e6f, 1e3f, line.put())) || FAILED(line->GetMetrics(&metrics))) return {0, 0};
    const std::pair<int, int> width{static_cast<int>(std::ceil(metrics.width)), static_cast<int>(std::ceil(metrics.widthIncludingTrailingWhitespace))};
    known.emplace(text, width);
    return width;
}

void NotesView::layout() {
    pieces_.clear();
    dots_.clear();
    content_ = 0;
    if (!window_) return;
    if (width_ <= 0 || !formatOf(Face::Body)) { InvalidateRect(window_, nullptr, FALSE); return; }
    // The scroll indicator's room is kept only when the notes do not fit. The last layout's choice is tried first, so a
    // resize usually lays out once; a narrower column never needs fewer lines, so fitting narrow means fitting wide.
    const int narrow = width_ - px(indicatorRoom);
    layoutAt(gutter_ ? narrow : width_);
    if (gutter_ && content_ <= height_) {
        gutter_ = false;
        layoutAt(width_);
    } else if (!gutter_ && content_ > height_) {
        gutter_ = true;
        layoutAt(narrow);
    }
    scrollTo(scroll_); // The content may have shrunk.
    InvalidateRect(window_, nullptr, FALSE);
}

// Places every block in a column `right` device pixels wide and sets the content height. Words are placed one by one
// and a line breaks before a word that would pass the column's right edge. A key stays whole; a word longer than the
// whole column is left to run past it rather than split.
void NotesView::layoutAt(int right) {
    pieces_.clear();
    dots_.clear();
    if (blocks_.empty()) pieces_.push_back({L"this build has no release notes.", Face::Body, false, theme::muted, 0, 0, right, px(22)});
    int y = blocks_.empty() ? px(22) : 0;
    for (size_t index = 0; index < blocks_.size(); ++index) {
        const auto& block = blocks_[index];
        using Kind = notes::Block::Kind;
        if (block.kind == Kind::Art) {
            const int line = px(18);
            for (size_t start = 0; start <= block.art.size();) {
                const auto end = std::min(block.art.find(L'\n', start), block.art.size());
                pieces_.push_back({block.art.substr(start, end - start), Face::Art, false, theme::accent, 0, y, right, line});
                y += line;
                start = end + 1;
            }
            y += px(18);
            continue;
        }
        const bool heading = block.kind == Kind::Heading, bullet = block.kind == Kind::Bullet;
        if (heading && index > 0) y += px(14);
        const int line = heading ? px(28) : px(22);
        const int left = bullet ? px(bulletIndent) : 0;
        if (bullet) dots_.push_back({px(7), y + line / 2});
        int x = left;
        for (const auto& span : block.spans) {
            using Style = notes::Span::Style;
            const bool key = span.style == Style::Key;
            const Face face = heading ? Face::Heading : span.style == Style::Bold ? Face::Bold :
                span.style == Style::Code ? Face::Code : Face::Body;
            // Bullets and headings in full ink; a plain paragraph ("changes since …") stays quiet.
            const COLORREF colour = heading || bullet || span.style != Style::Plain ? theme::ink : theme::muted;
            const auto place = [&](std::wstring text, int wordWidth, int advance) {
                if (x > left && x + wordWidth > right) { x = left; y += line; }
                pieces_.push_back({std::move(text), face, key, colour, x, y, wordWidth, line});
                x += advance;
            };
            if (key) {
                const int width = measure(span.text, face).first + 2 * px(keyPadding);
                x += px(keyGap);
                place(span.text, width, width + px(keyGap));
                continue;
            }
            // Each word with the spaces after it; the spaces count for the next word, never for the line's end.
            for (size_t start = 0; start < span.text.size();) {
                const auto wordEnd = std::min(span.text.find(L' ', start), span.text.size());
                const auto end = std::min(span.text.find_first_not_of(L' ', wordEnd), span.text.size());
                auto word = span.text.substr(start, end - start);
                const auto [wordWidth, advance] = measure(word, face);
                place(std::move(word), wordWidth, advance);
                start = end;
            }
        }
        y += line + (heading ? px(6) : bullet ? px(8) : px(14));
    }
    content_ = y;
}

void NotesView::scrollTo(int position) {
    const int previous = scroll_;
    scroll_ = std::clamp(position, 0, std::max(0, content_ - height_));
    if (scroll_ != previous) InvalidateRect(window_, nullptr, FALSE);
}

// Frames, bullets and the indicator with GDI, then every word in one DirectWrite pass over the same bitmap.
void NotesView::paint(HDC dc) {
    theme::fill(dc, {0, 0, width_, height_}, background_);
    for (const auto& piece : pieces_) {
        const int top = piece.y - scroll_;
        if (!piece.key || top + piece.height <= 0 || top >= height_) continue;
        theme::roundRect(dc, {piece.x, top + px(2), piece.x + piece.width, top + piece.height - px(2)}, px(5), theme::raised, theme::border);
    }
    for (const auto& dot : dots_) {
        const int y = dot.y - scroll_, r = px(2);
        if (y + r > 0 && y - r < height_) theme::roundRect(dc, {dot.x - r, y - r, dot.x + r, y + r}, r, theme::muted, theme::muted);
    }
    // A thin position indicator at the right, as on the Recording and Settings pages.
    if (const int range = content_ - height_; range > 0) {
        const int thumb = std::max(px(32), height_ * height_ / content_);
        const int top = (height_ - thumb) * scroll_ / range;
        theme::roundRect(dc, {width_ - px(4), top, width_, top + thumb}, px(2), theme::border, theme::border);
    }
    drawText(dc);
}

void NotesView::drawText(HDC dc) {
    if (!makeTarget()) return;
    const RECT bounds{0, 0, width_, height_};
    if (FAILED(target_->BindDC(dc, &bounds))) return;
    target_->BeginDraw();
    // One brush per colour in use (ink, muted, accent), made for this pass.
    std::vector<std::pair<COLORREF, winrt::com_ptr<ID2D1SolidColorBrush>>> brushes;
    const auto brushOf = [&](COLORREF colour) -> ID2D1SolidColorBrush* {
        for (const auto& [known, brush] : brushes) if (known == colour) return brush.get();
        winrt::com_ptr<ID2D1SolidColorBrush> brush;
        if (FAILED(target_->CreateSolidColorBrush(colourOf(colour), brush.put()))) return nullptr;
        brushes.emplace_back(colour, brush);
        return brush.get();
    };
    for (const auto& piece : pieces_) {
        const int top = piece.y - scroll_;
        if (top + piece.height <= 0 || top >= height_) continue;
        auto* brush = brushOf(piece.colour);
        if (!brush || piece.text.empty()) continue;
        // The measured width leaves out trailing spaces, so the box runs to the edge; a key's text sits inside its frame.
        const float left = static_cast<float>(piece.key ? piece.x + px(keyPadding) : piece.x);
        const D2D1_RECT_F box{left, static_cast<float>(top), static_cast<float>(width_), static_cast<float>(top + piece.height)};
        target_->DrawText(piece.text.c_str(), static_cast<UINT32>(piece.text.size()), formatOf(piece.face), box, brush,
            D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    }
    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) target_ = nullptr; // Made again on the next paint.
}

LRESULT NotesView::handle(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        width_ = LOWORD(lParam);
        height_ = HIWORD(lParam);
        layout();
        return 0;
    case WM_DPICHANGED_AFTERPARENT: // The dialog moved to a monitor with another scale and has taken its new size.
        setStyle(GetDpiForWindow(window_));
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        const HDC dc = BeginPaint(window_, &ps);
        buffer_.paint(dc, width_, height_, [&](HDC target) { paint(target); });
        EndPaint(window_, &ps);
        return 0;
    }
    case WM_MOUSEWHEEL:
        scrollTo(scroll_ - GET_WHEEL_DELTA_WPARAM(wParam) * px(72) / WHEEL_DELTA);
        return 0;
    case WM_KEYDOWN: {
        const int page = std::max(px(40), height_ - px(40));
        switch (wParam) {
        case VK_DOWN: scrollTo(scroll_ + px(40)); return 0;
        case VK_UP: scrollTo(scroll_ - px(40)); return 0;
        case VK_NEXT: scrollTo(scroll_ + page); return 0;
        case VK_PRIOR: scrollTo(scroll_ - page); return 0;
        case VK_HOME: scrollTo(0); return 0;
        case VK_END: scrollTo(content_); return 0;
        }
        break;
    }
    }
    return DefWindowProcW(window_, message, wParam, lParam);
}

LRESULT CALLBACK NotesView::proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* self = static_cast<NotesView*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<NotesView*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCDESTROY && self) { // The dialog that holds it is closing; the view may outlive the window.
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        self->window_ = nullptr;
        return DefWindowProcW(window, message, wParam, lParam);
    }
    return self ? self->handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

} // namespace refarter
