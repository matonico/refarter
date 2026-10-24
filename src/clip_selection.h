#pragma once

#include "clip_list.h"

#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace refarter {

// Which clips of the library grid are selected, kept by path so it survives a rescan or a change of tab.
// The grid's own list (the current tab, newest first) is passed in each time; anything that is not in
// it is never selected, so a bulk action can only reach clips listed in the current tab (a change of tab prunes
// the rest). Selected clips scrolled out of view are still included.
//
// Anchor rules: a plain click or a toggle sets the anchor to that clip; Shift+click and Shift+arrows select
// the range from the anchor to the clicked clip and leave the anchor where it is; with no anchor (nothing
// selected yet) they select just that clip and make it the anchor. Clearing drops the anchor too, and so
// does a rescan that no longer lists it.
class ClipSelection {
public:
    bool empty() const { return paths_.empty(); }
    std::size_t size() const { return paths_.size(); }
    bool contains(const std::filesystem::path& path) const { return paths_.count(path.wstring()) != 0; }
    const std::filesystem::path& anchor() const { return anchor_; }

    void clear() {
        paths_.clear();
        anchor_.clear();
    }

    // Plain click: only this clip, and the new anchor.
    void select(const std::vector<ClipFile>& list, int index) {
        if (!valid(list, index)) return;
        paths_.clear();
        paths_.insert(list[index].path.wstring());
        anchor_ = list[index].path;
    }

    // Ctrl+click: flips one clip and moves the anchor to it.
    void toggle(const std::vector<ClipFile>& list, int index) {
        if (!valid(list, index)) return;
        const auto key = list[index].path.wstring();
        if (!paths_.erase(key)) paths_.insert(key);
        anchor_ = list[index].path;
    }

    // Shift+click, Shift+arrows: the clips between the anchor and `index`, both included. `fallback` is the
    // clip to start from when there is no anchor (the keyboard place before a Shift+arrow). `add` (Ctrl+Shift)
    // keeps what was selected before.
    void range(const std::vector<ClipFile>& list, int index, int fallback = -1, bool add = false) {
        if (!valid(list, index)) return;
        int from = indexOf(list, anchor_);
        if (from < 0) {
            from = valid(list, fallback) ? fallback : index;
            anchor_ = list[from].path;
        }
        if (!add) paths_.clear();
        for (int i = std::min(from, index); i <= std::max(from, index); ++i) paths_.insert(list[i].path.wstring());
    }

    // Dragging a rectangle: adds one clip without touching the others.
    void include(const std::vector<ClipFile>& list, int index) {
        if (!valid(list, index)) return;
        paths_.insert(list[index].path.wstring());
        if (anchor_.empty()) anchor_ = list[index].path;
    }

    // Ctrl+A: everything in the current tab. An anchor that is still listed stays; otherwise the first clip.
    void selectAll(const std::vector<ClipFile>& list) {
        paths_.clear();
        for (const auto& clip : list) paths_.insert(clip.path.wstring());
        if (list.empty()) anchor_.clear();
        else if (indexOf(list, anchor_) < 0) anchor_ = list.front().path;
    }

    // After a rescan, a rename, a delete or a change of tab: forget clips that are no longer listed.
    void prune(const std::vector<ClipFile>& list) {
        std::unordered_set<std::wstring> listed;
        listed.reserve(list.size());
        for (const auto& clip : list) listed.insert(clip.path.wstring());
        for (auto it = paths_.begin(); it != paths_.end();) it = listed.count(*it) ? std::next(it) : paths_.erase(it);
        if (!anchor_.empty() && !listed.count(anchor_.wstring())) anchor_.clear();
        if (paths_.empty()) anchor_.clear();
    }

    // The selected clips in the list's order.
    std::vector<ClipFile> clips(const std::vector<ClipFile>& list) const {
        std::vector<ClipFile> result;
        for (const auto& clip : list)
            if (contains(clip.path)) result.push_back(clip);
        return result;
    }

private:
    static bool valid(const std::vector<ClipFile>& list, int index) { return index >= 0 && index < static_cast<int>(list.size()); }
    static int indexOf(const std::vector<ClipFile>& list, const std::filesystem::path& path) {
        if (path.empty()) return -1;
        const auto found = std::find_if(list.begin(), list.end(), [&](const ClipFile& clip) { return clip.path == path; });
        return found == list.end() ? -1 : static_cast<int>(found - list.begin());
    }

    std::unordered_set<std::wstring> paths_;
    std::filesystem::path anchor_;
};

// The clips a bulk "move to the recycle bin" may take, and those it must leave alone: a clip being
// compressed or open in the player is skipped, never recycled.
struct RecyclePlan {
    std::vector<ClipFile> bin;
    std::vector<ClipFile> skipped;
};

inline RecyclePlan planRecycle(const std::vector<ClipFile>& selected, const std::vector<std::filesystem::path>& inUse) {
    RecyclePlan plan;
    for (const auto& clip : selected) {
        const bool busy = std::find(inUse.begin(), inUse.end(), clip.path) != inUse.end();
        (busy ? plan.skipped : plan.bin).push_back(clip);
    }
    return plan;
}

// The confirmation for binning: the name of a single clip; for several, the names when there
// are at most five (otherwise the first three and "and N more"). Skipped clips are mentioned in the same
// dialog so that one answer covers everything. The question comes last and ends with a question mark.
inline std::wstring recycleQuestion(const std::vector<ClipFile>& bin, std::size_t skipped = 0) {
    constexpr std::size_t listAll = 5, listSome = 3;
    const std::wstring note = skipped ? std::to_wstring(skipped) + (skipped == 1 ? L" clip is" : L" clips are") +
        L" being compressed or open and will be left out." : std::wstring{};
    if (bin.size() == 1)
        return (note.empty() ? L"" : note + L"\n\n") + L"move “" + bin.front().path.filename().wstring() + L"” to the recycle bin?";
    // The names come first so that the dialog's own full stop is not added after a name.
    std::wstring text;
    const std::size_t shown = bin.size() <= listAll ? bin.size() : listSome;
    for (std::size_t i = 0; i < shown; ++i) text += L"“" + bin[i].path.filename().wstring() + L"”\n";
    if (shown < bin.size()) text += L"and " + std::to_wstring(bin.size() - shown) + L" more.\n";
    if (!note.empty()) text += L"\n" + note + L"\n";
    return text + L"\nmove " + std::to_wstring(bin.size()) + L" clips to the recycle bin?";
}

// The status line when binning skips clips without asking: "2 clips are being compressed or open and were left out".
inline std::wstring skippedNote(std::size_t skipped) {
    return std::to_wstring(skipped) + (skipped == 1 ? L" clip is" : L" clips are") + L" being compressed or open and " + (skipped == 1 ? L"was" : L"were") + L" left out";
}

// The status after copying: "copied 1 clip", "copied 3 clips".
inline std::wstring copiedNote(std::size_t count) {
    return L"copied " + std::to_wstring(count) + (count == 1 ? L" clip" : L" clips");
}

// The CF_HDROP block (a DROPFILES header, then each full path ending in a NUL, then one more NUL) as UTF-16.
// Empty paths are left out; with none left the result is empty.
inline std::vector<unsigned char> buildDropFiles(const std::vector<std::filesystem::path>& paths) {
    std::wstring list;
    for (const auto& path : paths) {
        if (path.empty()) continue;
        list += path.wstring();
        list.push_back(L'\0');
    }
    if (list.empty()) return {};
    list.push_back(L'\0');
    DROPFILES header{};
    header.pFiles = sizeof(DROPFILES);
    header.fWide = TRUE;
    std::vector<unsigned char> block(sizeof(DROPFILES) + list.size() * sizeof(wchar_t));
    std::copy_n(reinterpret_cast<const unsigned char*>(&header), sizeof(header), block.begin());
    std::copy_n(reinterpret_cast<const unsigned char*>(list.data()), list.size() * sizeof(wchar_t), block.begin() + sizeof(DROPFILES));
    return block;
}

// Puts the files on the clipboard as a file list marked "copy", so Explorer, Discord and the like paste
// them as files. The clipboard is opened (retried briefly while another program holds it) and always
// closed again; memory the clipboard did not take is freed. False when nothing was copied.
inline bool copyFilesToClipboard(HWND owner, const std::vector<std::filesystem::path>& paths) {
    const auto block = buildDropFiles(paths);
    if (block.empty()) return false;
    bool opened = false;
    for (int attempt = 0; attempt < 10 && !(opened = OpenClipboard(owner) != FALSE); ++attempt) Sleep(20);
    if (!opened) return false;
    bool copied = false;
    const UINT effectFormat = RegisterClipboardFormatW(L"Preferred DropEffect");
    HGLOBAL files = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, block.size());
    HGLOBAL effect = effectFormat ? GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD)) : nullptr;
    if (files) {
        if (void* target = GlobalLock(files)) {
            std::copy(block.begin(), block.end(), static_cast<unsigned char*>(target));
            GlobalUnlock(files);
            if (EmptyClipboard() && SetClipboardData(CF_HDROP, files)) {
                files = nullptr; // The clipboard owns it now.
                copied = true;
                if (effect) {
                    if (void* value = GlobalLock(effect)) {
                        *static_cast<DWORD*>(value) = DROPEFFECT_COPY;
                        GlobalUnlock(effect);
                        if (SetClipboardData(effectFormat, effect)) effect = nullptr;
                    }
                }
            }
        }
    }
    CloseClipboard();
    if (files) GlobalFree(files);
    if (effect) GlobalFree(effect);
    return copied;
}

// The shell's own data object for these files, the one Explorer hands out when it drags them (a file list, shell
// items and the rest), so anything that takes files dropped from Explorer takes these too. Paths that do not
// resolve are left out; null when none do. The caller releases it.
inline IDataObject* filesDataObject(const std::vector<std::filesystem::path>& paths) {
    std::vector<PCIDLIST_ABSOLUTE> items;
    for (const auto& path : paths) {
        PIDLIST_ABSOLUTE item = nullptr;
        if (!path.empty() && SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &item, 0, nullptr))) items.push_back(item);
    }
    IShellItemArray* array = nullptr;
    IDataObject* data = nullptr;
    if (!items.empty() && SUCCEEDED(SHCreateShellItemArrayFromIDLists(static_cast<UINT>(items.size()), items.data(), &array))) {
        if (FAILED(array->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data)))) data = nullptr;
        array->Release();
    }
    for (auto item : items) CoTaskMemFree(const_cast<PIDLIST_ABSOLUTE>(item));
    return data;
}

} // namespace refarter
