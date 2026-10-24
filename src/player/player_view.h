#pragma once

#include "player/toast_kind.h"

#include <d2d1_1.h>
#include <dwrite.h>
#include <winrt/base.h>
#include <map>
#include <string>
#include <vector>

namespace refarter::player {

// Interactive parts of the player surface.
enum class Part {
    None, Stage, Back, Play, Seek, Track, Mute, Volume, Trim, Folder, Fullscreen,
    Timeline, InHandle, OutHandle, Save, Replace, Cancel, CompressCopy, ErrorOpen, Rename, Title,
    Source0, Source1, Source2, Source3, Source4, Source5 // One icon switch per audio source.
};

// The source switches a clip can show; more sources are left out of the bar (the engine recognises desktop, microphone, discord, game and game output).
inline constexpr int maxSources = 6;
inline int sourceIndex(Part part) {
    return part >= Part::Source0 && part <= Part::Source5 ? static_cast<int>(part) - static_cast<int>(Part::Source0) : -1;
}

// What the interface shows; owned by the window (UI thread), copied to the engine.
struct View {
    float scale = 1;
    bool editing = false, fullscreen = false, controls = true;
    Part hover = Part::None, pressed = Part::None;
    double hoverTime = -1;      // Time under the mouse over the seek bar or timeline.
    double in = 0, out = 0;     // Trim range while editing.
    int volume = 100;
    bool muted = false;
    std::wstring title, date;   // The clip's name and the line the library shows under it.
    float titleWidth = 0;       // The title's width at scale 1 (measured by the window): clicking it renames.
    std::wstring toast;         // Short confirmation over the picture.
    ToastKind toastKind = ToastKind::Done; // Its icon.
    bool compressCopy = false;  // The trim bar's "compress & copy" box is ticked.
    bool compressBusy = false;  // A compression is running: the box is disabled.
    bool saving = false;
    bool compressing = false; // The saved trim is being compressed; keep its original trim view until completion.
    float progress = 0;
};

// What the engine learns about the open clip when it opens or its audio changes. Immutable once
// published, so the window and the painter share it without copying.
struct ClipInfo {
    bool ready = false;
    std::wstring error;
    double duration = 0, frameDuration = 1.0 / 60;
    int width = 16, height = 9;
    // Audio: the clip's tracks to pick one of (a pill and menu), or its sources to switch on and off (`toggles`: icons).
    std::vector<std::wstring> tracks;
    std::vector<bool> checked;
    std::vector<bool> included; // Sources that sound as part of another (the game inside desktop audio): not switchable.
    std::wstring audioLabel; // On the audio pill (plain multi-track files).
    bool toggles = false;    // `tracks` are named sources shown as icon switches, `checked` says which are on.
    // Audio a trim keeps: the switched-on sources (empty: every track), and whether they need a new mix.
    std::vector<int> trimTracks;
    bool trimMix = false;
    bool trimNone = false; // Every source is off: the trim keeps video only.
    std::vector<int> trimApart; // Kept tracks the new mix leaves out.
    std::vector<std::string> trimNames; // Their names, for clips that do not carry them.
    std::vector<double> keyframes;
};

// What changes while playing; small enough to copy on every poll.
struct Playback {
    double position = 0;
    bool playing = false;
};

struct Thumbnail {
    double time = 0;
    winrt::com_ptr<ID2D1Bitmap1> bitmap;
};

// Geometry in device pixels, shared by drawing and hit testing.
struct Layout {
    D2D1_RECT_F card{}, head{}, back{}, rename{}, title{}, stage{}, video{}, bar{}, seek{}, strip{}, controls{};
    D2D1_RECT_F group{}, sources[maxSources]{}; // The source switches and the bordered box around them.
    int sourceCount = 0;
    D2D1_RECT_F play{}, time{}, track{}, mute{}, volume{}, trim{}, folder{}, fullscreen{};
    D2D1_RECT_F save{}, replace{}, cancel{}, compress{}, errorOpen{};
    bool compressShort = false; // Too narrow for the full "compress & copy" label.
    float scale = 1;
};

Layout computeLayout(float width, float height, const View& view, const ClipInfo& info);
Part hitTest(const Layout& layout, const View& view, const ClipInfo& info, float x, float y);
// Time for an x position over the seek bar or the timeline strip.
double timeAt(const Layout& layout, const View& view, const ClipInfo& info, float x);
// Thumbnails that fill the timeline strip, and their size in pixels.
int thumbnailCount(const Layout& layout, const ClipInfo& info, float& width, float& height);

// Draws the whole surface. `picture` is the current frame converted to the
// video rectangle's size, or null before the first frame.
class Painter {
public:
    void draw(ID2D1DeviceContext* dc, IDWriteFactory* write, ID2D1Factory1* factory, const Layout& layout,
              const View& view, const ClipInfo& info, const Playback& playback, ID2D1Bitmap1* picture, const std::vector<Thumbnail>& thumbnails);

private:
    enum Geometry { Arrow, Pencil, Play, Speaker, WaveNear, WaveFar, StripClip, Geometries };
    void prepare(ID2D1DeviceContext* dc, ID2D1Factory1* factory, IDWriteFactory* write, const Layout& layout);
    float textWidth(IDWriteFactory* write, IDWriteTextFormat* format, const std::wstring& value);

    // Kept between passes: rebuilt only when the surface, scale or layout changes.
    winrt::com_ptr<IDWriteTextFormat> small_, date_, body_, strong_, title_;
    winrt::com_ptr<ID2D1SolidColorBrush> brush_;
    winrt::com_ptr<ID2D1StrokeStyle> round_;
    winrt::com_ptr<ID2D1LinearGradientBrush> shades_[2];
    winrt::com_ptr<ID2D1Geometry> geometry_[Geometries];
    std::map<std::pair<IDWriteTextFormat*, std::wstring>, float> widths_;
    ID2D1DeviceContext* dc_ = nullptr;
    Layout layout_{};
    float scale_ = 0;
};

} // namespace refarter::player
