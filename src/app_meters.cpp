// App: live audio level meters. The recorder (or, on the Recording page with replay off, a meter-only
// capture) feeds two LevelMeters; a 20 Hz timer exists only while a meter is on screen and repaints a
// bar only when its length or colour changes. The speaker and microphone icons beside the sidebar bars are
// static and drawn by the normal paint; the Recording page's sliders have a segmented meter instead.
#include "app_internal.h"
#include "capture/discord_audio.h"

#include <commctrl.h>
#include <algorithm>
#include <chrono>
#include <future>
#include <system_error>

namespace refarter {
namespace {
constexpr int meterInterval = 50;       // ms: 20 Hz.
constexpr int sidebarRows = 22;          // Expanded sidebar: one row per source, label then bar.
constexpr int sidebarRowsTop = 164;      // The rows start this far above the sidebar's foot.
constexpr int sidebarBarLeft = 46, sidebarBarThickness = 4;
constexpr int sliderIconSize = 16, sliderIconGap = 8; // The Recording page's icons sit between the meters and the sliders.
constexpr int meterIconSize = 12, sidebarIconLeft = 24, collapsedIconGap = 6;
constexpr int collapsedBarWidth = 6, collapsedBarGap = 8, collapsedBarTop = 206, collapsedBarBottom = 150; // Their icons follow, above the replay buttons.
constexpr UINT_PTR meterToolId = 0x4D54; // Tooltip tools: this and the next one (collapsed: the bars and icons; expanded: each icon).
} // namespace

bool App::meterDim(int source) const {
    const auto& device = source == 0 ? audio_.desktop : audio_.microphone;
    if (!device.enabled()) return true;
    if (recording_ == capture::RecorderState::Buffering) return source == 0 ? desktopMissing_ : microphoneMissing_;
    return !meterCapture_[source] || !meterCapture_[source]->active();
}

int App::meterFill(int source, int length, int thickness) const {
    if (meterDim(source)) return 0;
    const int filled = capture::meterLength(meterBars_[source].fraction(), length);
    return filled ? std::max(filled, std::min(thickness, length)) : 0; // Never thinner than its rounded ends.
}

// The bar of one display, in pixels: 0/1 the sidebar's desktop and microphone, 2/3 the segmented meter's box
// left of the volume sliders (empty while the slider is hidden). All in window coordinates.
RECT App::meterRect(int display) const {
    const int source = display & 1;
    if (display < 2) {
        const int bottom = footerTop();
        if (state_.sidebarCollapsed) {
            const int x = (collapsedSidebar - (2 * collapsedBarWidth + collapsedBarGap)) / 2 + source * (collapsedBarWidth + collapsedBarGap);
            return {px(x), px(bottom - collapsedBarTop), px(x + collapsedBarWidth), px(bottom - collapsedBarBottom)};
        }
        const int y = bottom - sidebarRowsTop + source * sidebarRows + (sidebarRows - sidebarBarThickness) / 2;
        return {px(sidebarBarLeft), px(y), px(sidebarWidth() - 24), px(y + sidebarBarThickness)};
    }
    const HWND slider = control(source == 0 ? DesktopVolume : MicrophoneVolume);
    RECT rect{}, thumb{};
    if (!slider || !IsWindowVisible(slider)) return {};
    GetWindowRect(slider, &rect);
    MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&rect), 2);
    SendMessageW(slider, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    // Centred on the slider's track (the knob's middle, as drawVolumeSlider draws it), which the layout puts on the row's middle.
    const int middle = rect.top + (thumb.top + thumb.bottom) / 2, left = rect.left - px(meterSliderGap) - px(meterBoxWidth);
    const int top = middle - px(meterBoxHeight) / 2;
    return {left, top, left + px(meterBoxWidth), top + px(meterBoxHeight)};
}

// The speaker or microphone icon of one sidebar display, in pixels.
RECT App::meterIconRect(int display) const {
    const int source = display & 1, size = px(meterIconSize);
    if (display < 2) {
        if (state_.sidebarCollapsed) {
            const RECT bar = meterRect(display); // Centred under its vertical bar.
            const int left = (bar.left + bar.right - size) / 2, top = bar.bottom + px(collapsedIconGap);
            return {left, top, left + size, top + size};
        }
        const int bottom = footerTop();
        const int top = px(bottom - sidebarRowsTop + source * sidebarRows) + (px(sidebarRows) - size) / 2;
        return {px(sidebarIconLeft), top, px(sidebarIconLeft) + size, top + size};
    }
    // A slider's icon, in window coordinates: left of the slider, centred on the same line as the meter.
    const RECT box = meterRect(display);
    if (box.right <= box.left) return {};
    const HWND slider = control(source == 0 ? DesktopVolume : MicrophoneVolume);
    RECT rect{};
    GetWindowRect(slider, &rect);
    MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&rect), 2);
    const int bigger = px(sliderIconSize), middle = (box.top + box.bottom) / 2;
    const int left = rect.left - px(sliderIconGap) - bigger, top = middle - bigger / 2;
    return {left, top, left + bigger, top + bigger};
}
void App::drawMeterIcon(HDC dc, int display) const {
    const RECT r = meterIconRect(display);
    icon(dc, (display & 1) == 0 ? meterSpeakerIcon : meterMicIcon, r.left, r.top, r.right - r.left, muted);
}

void App::drawMeterBar(HDC dc, RECT bar, bool vertical, int source) const {
    const bool dim = meterDim(source);
    const int thickness = vertical ? bar.right - bar.left : bar.bottom - bar.top;
    const int length = vertical ? bar.bottom - bar.top : bar.right - bar.left;
    if (length <= 0 || thickness <= 0) return;
    roundRect(dc, bar, thickness / 2, dim ? raised : border, dim ? raised : border);
    const int filled = meterFill(source, length, thickness);
    if (!filled) return;
    const COLORREF value = meterBars_[source].clipping() ? accent : muted;
    RECT part = bar;
    if (vertical) part.top = bar.bottom - filled;
    else part.right = bar.left + filled;
    roundRect(dc, part, thickness / 2, value, value);
}

// Sidebar labels and bars, drawn from paint() while replay is buffering.
void App::paintSidebarMeters(HDC dc, const RECT& dirty) const {
    if (recording_ != capture::RecorderState::Buffering) return;
    const int bottom = footerTop();
    const RECT area{0, px(bottom - collapsedBarTop - 4), px(sidebarWidth()), px(bottom - 116)};
    RECT hit;
    if (!IntersectRect(&hit, &dirty, &area)) return;
    for (int source = 0; source < 2; ++source) {
        drawMeterIcon(dc, source);
        drawMeterBar(dc, meterRect(source), state_.sidebarCollapsed, source);
    }
}

// How many of a segmented meter's ten bars are lit: the level rounded to the nearest bar (with the -60 dB floor the
// tenth lights from -3 dB, where meterClipDb turns the sidebar bar to the warning colour).
int App::litSegments(int source) const {
    if (meterDim(source)) return 0;
    const float level = meterBars_[source].fraction();
    return std::clamp(static_cast<int>(std::lround(level * static_cast<float>(meterSegments))), 0, meterSegments);
}

// Ten bottom-aligned bars growing from 7 to about 17 px; the last two light up in a paler colour.
void App::drawSegmentMeter(HDC dc, RECT box, int source) const {
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const int lit = litSegments(source);
    const double scale = static_cast<double>(dpi_) / 96.0;
    for (int i = 0; i < meterSegments; ++i) {
        const int left = box.left + px(i * (meterSegmentWidth + meterSegmentGap));
        const int height = static_cast<int>(std::lround((7 + i * 1.1) * scale));
        const COLORREF value = i >= lit ? border : i >= meterSegments - 2 ? peak : accent;
        roundRect(graphics, {left, box.bottom - height, left + px(meterSegmentWidth), box.bottom}, px(2), value, value);
    }
}

// The sliders' meters and icons, painted with the page (they sit outside the slider windows).
void App::paintSliderMeters(HDC dc, const RECT& dirty) const {
    for (int display = 2; display < 4; ++display) {
        const RECT r = meterRect(display);
        RECT hit;
        if (r.right > r.left && IntersectRect(&hit, &dirty, &r)) drawSegmentMeter(dc, r, display & 1);
        const RECT i = meterIconRect(display);
        if (i.right > i.left && IntersectRect(&hit, &dirty, &i)) drawMeterIcon(dc, display);
    }
}

void App::updateMeterTooltip() {
    if (!tooltips_) return;
    const bool shown = recording_ == capture::RecorderState::Buffering;
    for (UINT_PTR index = 0; index < 2; ++index) {
        TOOLINFOW tool{sizeof(tool)};
        tool.hwnd = window_;
        tool.uId = meterToolId + index;
        if (shown && state_.sidebarCollapsed) {
            if (index == 0) { // One tip over both bars and their icons.
                const RECT a = meterRect(0), b = meterRect(1), icon = meterIconRect(0);
                tool.rect = {std::min(a.left, b.left) - px(4), a.top, std::max(a.right, b.right) + px(4), icon.bottom};
            }
        } else if (shown) { // Expanded: each icon names its source.
            const RECT icon = meterIconRect(static_cast<int>(index));
            tool.rect = {icon.left - px(2), icon.top - px(2), icon.right + px(2), icon.bottom + px(2)};
        }
        if (!meterToolAdded_) {
            tool.uFlags = TTF_SUBCLASS;
            tool.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(tooltips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
        SendMessageW(tooltips_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
    }
    meterToolAdded_ = true;
}
bool App::isMeterTool(UINT_PTR id) const { return id == meterToolId || id == meterToolId + 1; }

const wchar_t* App::meterToolText(UINT_PTR id) const {
    if (state_.sidebarCollapsed) return L"desktop · mic";
    return id == meterToolId ? L"desktop audio" : L"microphone";
}

// Stops one meter-only capture without blocking the UI: its thread is joined (and the device released) by a
// background task. The meter is detached first so a stale packet never reaches it; the meters are App members
// and every task is joined in ~App, so none outlives them. Reopening the same device meanwhile is fine: each
// capture holds its own shared-mode client.
void App::closeMeterCapture(int source) {
    auto capture = std::move(meterCapture_[source]);
    if (!capture) return;
    capture->detachMeter();
    std::erase_if(meterClosers_, [](auto& closer) { return closer.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
    try {
        meterClosers_.push_back(std::async(std::launch::async, [moved = std::move(capture)]() mutable { moved.reset(); }));
    } catch (const std::system_error&) {
        // No thread to spare: the task's copy was destroyed while unwinding, which closed the capture right here.
    }
}
void App::closeMeterCaptures() {
    for (int source = 0; source < 2; ++source) closeMeterCapture(source);
}
void App::joinMeterClosers() {
    meterClosers_.clear(); // A future from std::async waits for its task when destroyed.
}

// Decides what runs: the meter-only captures (Recording page, replay off) and the timer (any meter on screen).
// Called whenever the page, window visibility, recording state or an audio choice changes.
void App::syncMeters(bool reopen) {
    const bool shown = window_ && !closing_ && IsWindowVisible(window_) && !IsIconic(window_);
    const bool onPage = shown && state_.page == Page::Recording && !fullscreen_.on;
    const bool buffering = recording_ == capture::RecorderState::Buffering;
    const bool idle = onPage && recording_ == capture::RecorderState::Off && !busy_;
    for (int source = 0; source < 2; ++source) {
        const auto& device = source == 0 ? audio_.desktop : audio_.microphone;
        if (reopen || !idle || !device.enabled()) closeMeterCapture(source);
        if (!idle || !device.enabled() || meterCapture_[source]) continue;
        // Mirrors the recorder: system-default desktop audio leaves Discord out where it has its own track.
        capture::AudioCapture::ProcessTarget target;
        if (source == 0 && device.mode == capture::AudioDevice::Mode::SystemDefault && !capture::endpointLoopbackForced())
            target = [pid = capture::findDiscordProcess()] { return pid; };
        meterCapture_[source] = std::make_unique<capture::AudioCapture>(
            source == 0 ? capture::AudioCapture::Device::Desktop : capture::AudioCapture::Device::Microphone,
            device.mode == capture::AudioDevice::Mode::Fixed ? device.id : std::wstring{}, nullptr,
            std::function<void()>{}, std::move(target), source == 0 ? &desktopMeter_ : &microphoneMeter_);
    }
    // A bar is on screen while the sidebar shows with replay buffering, or on the Recording page. The fullscreen
    // player covers both, as does a hidden or minimised window; the library page with replay off shows none. And a
    // bar only moves for a source that is on (and, while buffering, not reported missing): with both off or missing
    // every bar stays dim and there is nothing to animate.
    const auto moves = [&](int source) {
        const auto& device = source == 0 ? audio_.desktop : audio_.microphone;
        return device.enabled() && !(buffering && (source == 0 ? desktopMissing_ : microphoneMissing_));
    };
    const bool timer = shown && !fullscreen_.on && (buffering || onPage) && (moves(0) || moves(1));
    desktopMeter_.want(timer); // Capture threads scan peaks only while a meter is visible.
    microphoneMeter_.want(timer);
    if (timer == meterTimer_) return;
    meterTimer_ = timer;
    meterBars_ = {};
    meterKeys_.fill(-1);
    desktopMeter_.take(); // Whatever arrived while nothing was shown is stale.
    microphoneMeter_.take();
    if (timer) {
        meterTick_ = GetTickCount64();
        if (!SetTimer(window_, meterTimer, meterInterval, nullptr)) meterTimer_ = false;
    } else {
        KillTimer(window_, meterTimer);
    }
}

// Redraws one meter on screen without invalidating the window (a normal paint of the sidebar or a slider
// costs far more than the few pixels that moved). The whole meter area, a little beyond the bar so that no
// anti-aliased edge of an older bar survives, is composed from its true background in a kept off-screen
// bitmap and copied in one go: the sidebar's colour, or the card's for a slider's meter. The next full paint
// draws the same pixels.
void App::paintMeterNow(int display, HWND target, RECT bar, bool vertical, int source) {
    const int pad = px(2);
    RECT client, area{bar.left - pad, bar.top - pad, bar.right + pad, bar.bottom + pad};
    GetClientRect(target, &client);
    if (!IntersectRect(&area, &area, &client)) return;
    const int width = area.right - area.left, height = area.bottom - area.top;
    const HDC screen = GetDC(target);
    if (!screen) return;
    auto& buffer = meterBuffers_[static_cast<size_t>(display)];
    if (const HDC dc = buffer.acquire(screen, width, height)) {
        fill(dc, {0, 0, width, height}, display < 2 ? sidebar : surface); // A slider's meter sits on the page's card.
        const RECT local{bar.left - area.left, bar.top - area.top, bar.right - area.left, bar.bottom - area.top};
        if (display < 2) drawMeterBar(dc, local, vertical, source);
        else drawSegmentMeter(dc, local, source);
        BitBlt(screen, area.left, area.top, width, height, dc, 0, 0, SRCCOPY);
    }
    ReleaseDC(target, screen);
}
void App::stepMeters() {
    if (!meterTimer_) return;
    const ULONGLONG now = GetTickCount64();
    const float seconds = std::clamp(static_cast<float>(now - meterTick_) / 1000.f, 0.001f, 0.25f);
    meterTick_ = now;
    const int peaks[2] = {desktopMeter_.take(), microphoneMeter_.take()};
    const int percents[2] = {audio_.levels.desktopPercent, audio_.levels.microphonePercent};
    for (int source = 0; source < 2; ++source) meterBars_[source].update(capture::peakToDb(peaks[source], percents[source]), seconds);
    const bool buffering = recording_ == capture::RecorderState::Buffering;
    for (int display = 0; display < 4; ++display) {
        const int source = display & 1;
        RECT bar{};
        HWND target = window_;
        bool vertical = false;
        if (display < 2) {
            if (!buffering) continue;
            vertical = state_.sidebarCollapsed;
        } else {
            const HWND slider = control(source == 0 ? DesktopVolume : MicrophoneVolume);
            if (state_.page != Page::Recording || !slider || !IsWindowVisible(slider)) continue;
        }
        bar = meterRect(display);
        const int thickness = vertical ? bar.right - bar.left : bar.bottom - bar.top;
        const int length = vertical ? bar.bottom - bar.top : bar.right - bar.left;
        const int key = display < 2 ? meterFill(source, length, thickness) | (meterBars_[source].clipping() << 20) | (meterDim(source) << 21)
                                    : litSegments(source);
        if (key == meterKeys_[display]) continue;
        meterKeys_[display] = key;
        paintMeterNow(display, target, bar, vertical, source);
    }
}

} // namespace refarter
