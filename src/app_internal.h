#pragma once

// Constants and helpers shared by the app*.cpp files that implement App. Include only from them.

#include "app.h"

#include <algorithm>

namespace refarter {
namespace internal {
using namespace theme;
using namespace ui;

// Sidebar widths and the gap before page content, in logical pixels.
constexpr int expandedSidebar = 214, collapsedSidebar = 64, contentGap = 36;
// A full-width footer under the sidebar and pages: status and source on the left, the version on the right.
constexpr int footerHeight = 26;
// Window messages shared by several app*.cpp files. WM_APP + 1 to + 6 and + 8 are private to app.cpp, + 3 and + 4 also in auto_session.h.
constexpr UINT leftoversDone = WM_APP + 7;   // The startup scan for unfinished files has ended (wParam found, lParam moved).
constexpr UINT updateChecked = WM_APP + 9;   // The release check's worker has an answer (UpdateClient::take; see app_updates.cpp).
constexpr UINT updateQuestion = WM_APP + 10; // Ask the opt-in question now if Refarter is in front.
constexpr UINT errorDialogs = WM_APP + 11;   // Show the error messages that waited for Refarter's own window to be in front.
// The right end of the footer reserves this much (logical px) for the version text; an "update available" text adds its own width.
constexpr int footerVersionReserve = 220;
constexpr std::chrono::seconds startupUpdateDelay{30}; // The automatic check waits this long after start-up (a worker asleep on an event, not a timer).
// Library: the filter row, then the clip card, which ends this far above the footer.
// The strip below the card holds the folder button at its right edge and the folder size (or a short note) before it.
constexpr int libraryRowTop = 80, libraryRowHeight = 38, libraryCardTop = 118, libraryCardBottom = 46;
// The tile-size buttons sit 6 px above the card's border.
constexpr int tileButton = 32, tileButtonTop = libraryCardTop - 6 - tileButton;
// The pair of tile-size buttons ends at the content edge; the clip count ends before them.
constexpr int tileGap = 4, countGap = 16, countWidth = 90;
// The folders toggle stands a little apart, left of the tile-size pair; the clip count then ends before it.
constexpr int folderGap = 10, libraryButtonsWidth = 3 * tileButton + tileGap + folderGap;
constexpr int footerButton = 30; // The strip's folder button.
// The library's search field, on the line of the page title: the edit, its frame's padding and the magnifier before it.
constexpr int searchWidth = 200, searchHeight = 23, searchTop = 29, searchFramePad = 8, searchIconRoom = 22;
// The logo: a rounded mark, and the app name beside it while the sidebar is expanded (logical pixels).
constexpr int logoTop = 29, logoSize = 34, logoMarkInset = 4, logoIconSize = 26, logoNameLeft = 70, logoNameWidth = 92;
inline int logoLeft(bool collapsed) { return collapsed ? 15 : 24; }
// The library's view tabs, in order, and the divider between two neighbours (shown when neither is chosen).
constexpr int viewTabs[] = {AllClips, Favourites, Compressed};
inline int tabFor(ClipView view) { return viewTabs[static_cast<int>(view)]; }
inline bool tabDivider(ClipView view, int left) { return left != static_cast<int>(view) && left + 1 != static_cast<int>(view); }
// Hand-drawn icon of a nav page (the page's control id) or, with the negative kinds, a meter's source; defined in app_paint.cpp.
constexpr int meterSpeakerIcon = -1, meterMicIcon = -2;
void icon(HDC dc, int kind, int x, int y, int size, COLORREF value);
// Library buttons drawn as icons; their text is the tooltip and the screen reader name.
constexpr int libraryIconIds[] = {GroupFolders, LargeTiles, SmallTiles, OpenFolder};
constexpr int numberIds[] = {CustomSeconds, Bitrate, StorageLimitField, GateOpen, GateClose, GateAttack, GateHold, GateRelease};
constexpr int sliderIds[] = {DesktopVolume, MicrophoneVolume, ChimeVolume};
// The percentages beside the source sliders can be typed in; the chime's stays a plain label.
constexpr int typedVolumeIds[] = {DesktopVolumeLabel, MicrophoneVolumeLabel};
// A mouse button going down, in the client area or not.
constexpr bool isMousePress(UINT message) {
    return message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN ||
           message == WM_NCLBUTTONDOWN || message == WM_NCRBUTTONDOWN || message == WM_NCMBUTTONDOWN;
}
// A slider and the percentage beside it.
constexpr int volumeLabelOf(int slider) { return slider == DesktopVolume ? DesktopVolumeLabel : slider == MicrophoneVolume ? MicrophoneVolumeLabel : ChimeVolumeLabel; }
constexpr int volumeSliderOf(int label) { return label == DesktopVolumeLabel ? DesktopVolume : label == MicrophoneVolumeLabel ? MicrophoneVolume : ChimeVolume; }
template <size_t N> bool contains(const int (&ids)[N], int id) { return std::find(std::begin(ids), std::end(ids), id) != std::end(ids); }
// Geometry of the Recording and Settings pages, in logical pixels.
constexpr int pageTop = 24;       // Margin kept above and below when scrolling to a control.
constexpr int headingHeight = 32; // Section title above its card.
// Space a row control takes at the end of its row: its size, the gap
// before it (number fields: their frame and caption) and the gap after it.
struct Slot { int width, height, lead, trail; };
// The segmented level meter left of a source slider: ten bars, then the gap before the slider (the slider's lead).
constexpr int meterSegments = 10, meterSegmentWidth = 5, meterSegmentGap = 2, meterBoxHeight = 18, meterSliderGap = 36; // The gap covers 12 px, the 16 px icon and 8 px more.
constexpr int meterBoxWidth = meterSegments * meterSegmentWidth + (meterSegments - 1) * meterSegmentGap;
inline Slot rowSlot(int id) {
    switch (id) {
    case DesktopAudio: case Microphone: return {260, 34, 0, 0};
    // Reset buttons sit 8 after the device menu.
    case DesktopReset: case MicrophoneReset: return {34, 34, 8, 0};
    case Resolution: case FrameRate: case QualityPreset: case VideoCodec: return {150, 34, 0, 0};
    case AutoCaptureSource: return {150, 34, 0, 0};
    case DesktopVolume: case MicrophoneVolume: return {150, 24, meterBoxWidth + meterSliderGap, 12}; // The lead holds the meter.
    case ChimeVolume: return {220, 24, 0, 12}; // Widest; narrows in small windows.
    case DesktopVolumeLabel: case MicrophoneVolumeLabel: case ChimeVolumeLabel: return {48, 20, 0, 16};
    // Number fields: the trail holds the frame and the unit painted inside it (see fieldUnit).
    case CustomSeconds: return {48, 23, 8, 20};
    case Bitrate: return {40, 23, 8, 42};
    case StorageLimitField: return {52, 23, 8, 28};
    case StorageBehaviour: return {200, 34, 0, 0};
    case GateOpen: case GateClose: return {40, 23, 60, 28};
    case GateAttack: case GateHold: case GateRelease: return {40, 23, 60, 30};
    case Suppression: case Gate: case RecordDiscord: case CloseToTray: case StartWithWindows: case StartMinimised: case DiagnosticLogging: case CompressCopyAfterTrim: case AskBeforeBinning: case UpdateCheckSwitch: return {40, 22, 0, 0};
    case UpdateCheckNow: return {160, 34, 0, 16}; // Left of the switch, 16 px away.
    case ThirtySeconds: case OneMinute: case ThreeMinutes: case FiveMinutes: return {54, 34, 4, 0};
    case SelectProcess: return {130, 34, 0, 0};
    default: return {140, 34, 0, 0};
    }
}
// The unit shown inside a number field's frame, after the value, and the room it takes.
struct Unit { const wchar_t* text; int width; };
inline Unit fieldUnit(int id) {
    switch (id) {
    case GateOpen: case GateClose: return {L"db", 16};
    case GateAttack: case GateHold: case GateRelease: return {L"ms", 18};
    case Bitrate: return {L"mbps", 30};
    case StorageLimitField: return {L"gb", 16};
    case CustomSeconds: return {L"s", 8};
    default: return {L"", 0};
    }
}
constexpr UINT_PTR exportTimer = 2; // Exists only while a compression job is running.
constexpr UINT_PTR switchTimer = 3; // Exists only while a switch animates.
constexpr UINT_PTR meterTimer = 4;  // Exists only while a level meter is on screen.
constexpr UINT_PTR resumeTimer = 5; // One shot after a resume from sleep, while devices settle.
constexpr UINT_PTR bubbleTimer = 6; // One shot: hides the slider value bubble after the last key.
constexpr UINT_PTR gateBubbleTimer = 7; // One shot: hides the gate's error bubble a few seconds after it appears.
// The global shortcuts register under their index + 1 (see saveShortcut and screenshotShortcut in app.h).
static_assert(RawHotkey::count == shortcutCount, "Raw Input must track every global shortcut");

} // namespace internal
using namespace internal;
} // namespace refarter
