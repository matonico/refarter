#include "clip_selection.h"
#include <shellapi.h>
#include <cstring>
#include <fstream>
#include <iostream>

using namespace refarter;

namespace {
int failures = 0;
void check(bool passes, const char* name) {
    if (!passes) { std::cerr << name << '\n'; ++failures; }
}

std::vector<ClipFile> clips(std::initializer_list<const wchar_t*> names) {
    std::vector<ClipFile> list;
    for (const auto* name : names) list.push_back({std::filesystem::path(L"d:\\clips") / name, {}, 1});
    return list;
}
std::filesystem::path at(const std::vector<ClipFile>& list, int i) { return list[i].path; }
std::vector<std::wstring> names(const std::vector<ClipFile>& list) {
    std::vector<std::wstring> result;
    for (const auto& clip : list) result.push_back(clip.path.filename().wstring());
    return result;
}
} // namespace

int main() {
    const auto list = clips({L"a.mp4", L"b.mp4", L"c.mp4", L"d.mp4", L"e.mp4", L"f.mp4"});

    ClipSelection s;
    check(s.empty() && s.size() == 0, "starts empty");
    s.select(list, 2);
    check(s.size() == 1 && s.contains(at(list, 2)) && s.anchor() == at(list, 2), "click selects one and anchors");
    s.select(list, 4);
    check(s.size() == 1 && !s.contains(at(list, 2)) && s.contains(at(list, 4)), "a second click replaces");
    s.select(list, 9);
    s.select(list, -1);
    check(s.size() == 1 && s.contains(at(list, 4)), "out of range ignored");

    s.toggle(list, 1);
    check(s.size() == 2 && s.contains(at(list, 1)) && s.contains(at(list, 4)) && s.anchor() == at(list, 1), "ctrl+click adds and moves the anchor");
    s.toggle(list, 4);
    check(s.size() == 1 && !s.contains(at(list, 4)), "ctrl+click removes");
    s.toggle(list, 1);
    check(s.empty() && s.anchor() == at(list, 1), "toggling the last one off leaves an empty selection");

    s.select(list, 1);
    s.range(list, 3);
    check(s.size() == 3 && s.contains(at(list, 1)) && s.contains(at(list, 2)) && s.contains(at(list, 3)) && s.anchor() == at(list, 1),
          "shift+click selects the range, anchor stays");
    s.range(list, 0);
    check(s.size() == 2 && s.contains(at(list, 0)) && s.contains(at(list, 1)) && !s.contains(at(list, 2)), "range upward shrinks and flips from the anchor");
    s.select(list, 5);
    s.toggle(list, 0);
    s.range(list, 2, -1, true);
    check(s.contains(at(list, 5)) && s.contains(at(list, 0)) && s.contains(at(list, 1)) && s.contains(at(list, 2)) && !s.contains(at(list, 3)),
          "ctrl+shift keeps the earlier selection");

    ClipSelection fresh;
    fresh.range(list, 3);
    check(fresh.size() == 1 && fresh.contains(at(list, 3)) && fresh.anchor() == at(list, 3), "range without an anchor selects one and anchors");
    ClipSelection keys;
    keys.range(list, 3, 1); // Shift+arrows from the focused clip 1 down to 3.
    check(keys.size() == 3 && keys.anchor() == at(list, 1) && keys.contains(at(list, 1)) && keys.contains(at(list, 3)), "keyboard range starts at the previous focus");
    keys.range(list, 4, 1);
    check(keys.size() == 4 && keys.anchor() == at(list, 1), "extending keeps the anchor");

    s.selectAll(list);
    check(s.size() == list.size(), "select all");
    ClipSelection all;
    all.selectAll(list);
    check(all.size() == 6 && all.anchor() == at(list, 0), "select all anchors the first clip when there was none");
    all.selectAll({});
    check(all.empty() && all.anchor().empty(), "select all of nothing");

    s.clear();
    check(s.empty() && s.anchor().empty(), "clear drops selection and anchor");

    // Rescan, rename, delete, change of tab: only listed clips stay selected.
    ClipSelection p;
    p.select(list, 0);
    p.toggle(list, 2);
    p.toggle(list, 4);
    auto fewer = clips({L"a.mp4", L"c.mp4", L"x.mp4"}); // b, d, e, f are gone; c and a remain.
    p.prune(fewer);
    check(p.size() == 2 && p.contains(L"d:\\clips\\a.mp4") && p.contains(L"d:\\clips\\c.mp4") && !p.contains(L"d:\\clips\\e.mp4"), "prune drops vanished clips");
    check(p.anchor().empty(), "prune drops a vanished anchor");
    p.select(fewer, 1);
    p.prune(fewer);
    check(p.anchor() == fewer[1].path, "prune keeps a listed anchor");
    p.prune(clips({L"zzz.mp4"}));
    check(p.empty() && p.anchor().empty(), "prune of an unrelated list empties");
    // A renamed clip is a different path: the old selection never carries over to a name it does not have.
    ClipSelection renamed;
    renamed.select(list, 1);
    renamed.prune(clips({L"a.mp4", L"b renamed.mp4", L"c.mp4"}));
    check(renamed.empty(), "a renamed clip is dropped");
    ClipSelection tab;
    tab.selectAll(list);
    tab.prune(clips({L"b.mp4", L"d.mp4"})); // Another tab shows only some clips.
    check(tab.size() == 2 && names(tab.clips(clips({L"b.mp4", L"d.mp4"}))) == std::vector<std::wstring>{L"b.mp4", L"d.mp4"}, "tab change keeps only what is shown");
    check(tab.clips(list).size() == 2, "clips() returns selected in list order only");
    ClipSelection ordered;
    ordered.select(list, 4);
    ordered.toggle(list, 0);
    const auto got = names(ordered.clips(list));
    check(got.size() == 2 && got[0] == L"a.mp4" && got[1] == L"e.mp4", "selected clips come back in list order");

    // Bulk recycle: only the selection, never the clip being compressed or open.
    const auto plan = planRecycle(list, {list[1].path});
    check(plan.bin.size() == 5 && plan.skipped.size() == 1 && plan.skipped[0].path == list[1].path, "a busy clip is skipped");
    for (const auto& clip : plan.bin) check(clip.path != list[1].path, "busy clip is not in the bin list");
    check(planRecycle({}, {list[0].path}).bin.empty(), "nothing selected, nothing binned");
    const auto onlyBusy = planRecycle(clips({L"a.mp4"}), {L"d:\\clips\\a.mp4"});
    check(onlyBusy.bin.empty() && onlyBusy.skipped.size() == 1, "only a busy clip leaves nothing to bin");

    // Confirmation text: the question comes last (the dialog adds a full stop after anything else).
    check(recycleQuestion(clips({L"a.mp4"})) == L"move “a.mp4” to the recycle bin?", "single clip question keeps the old wording");
    const auto three = recycleQuestion(clips({L"a.mp4", L"b.mp4", L"c.mp4"}));
    check(three.find(L"\nmove 3 clips to the recycle bin?") != std::wstring::npos && three.back() == L'?' &&
          three.find(L"“a.mp4”") != std::wstring::npos && three.find(L"“c.mp4”") != std::wstring::npos &&
          three.find(L"more") == std::wstring::npos, "up to five names are listed");
    const auto five = recycleQuestion(clips({L"a.mp4", L"b.mp4", L"c.mp4", L"d.mp4", L"e.mp4"}));
    check(five.find(L"“e.mp4”") != std::wstring::npos && five.find(L"more") == std::wstring::npos, "five names are all listed");
    const auto six = recycleQuestion(list);
    check(six.find(L"move 6 clips to the recycle bin?") != std::wstring::npos && six.find(L"and 3 more") != std::wstring::npos &&
          six.find(L"“c.mp4”") != std::wstring::npos && six.find(L"“d.mp4”") == std::wstring::npos, "more than five: three names and 'and N more'");
    check(recycleQuestion(clips({L"a.mp4", L"b.mp4"}), 2).find(L"2 clips are being compressed or open") != std::wstring::npos, "skipped clips are mentioned");
    check(recycleQuestion(clips({L"a.mp4", L"b.mp4"}), 1).find(L"1 clip is being") != std::wstring::npos, "one skipped clip");
    const auto left = recycleQuestion(clips({L"a.mp4"}), 1);
    check(left.find(L"1 clip is being") != std::wstring::npos && left.back() == L'?', "one binned, one skipped: both said, question last");
    check(recycleQuestion(clips({L"a.mp4"})).back() == L'?' && recycleQuestion(list).back() == L'?', "ends with a question mark");
    check(skippedNote(1) == L"1 clip is being compressed or open and was left out" &&
          skippedNote(2) == L"2 clips are being compressed or open and were left out", "skipped note");
    check(copiedNote(1) == L"copied 1 clip" && copiedNote(3) == L"copied 3 clips", "copied note");

    // CF_HDROP block: header, then NUL-separated UTF-16 paths and a closing NUL.
    check(buildDropFiles({}).empty() && buildDropFiles({L""}).empty(), "no paths, no block");
    const std::filesystem::path one(L"d:\\clips\\caf\u00e9 \u30af\u30ea\u30c3\u30d7 \U0001F430.mp4"), two(L"d:\\clips\\b.mp4");
    const auto block = buildDropFiles({one, {}, two});
    DROPFILES header{};
    check(block.size() > sizeof(DROPFILES), "block has a header and paths");
    std::memcpy(&header, block.data(), sizeof(header));
    check(header.pFiles == sizeof(DROPFILES) && header.fWide == TRUE, "wide DROPFILES header");
    const auto* text = reinterpret_cast<const wchar_t*>(block.data() + header.pFiles);
    const std::size_t count = (block.size() - header.pFiles) / sizeof(wchar_t);
    std::wstring expected = one.wstring();
    expected.push_back(L'\0');
    expected += two.wstring();
    expected.push_back(L'\0');
    expected.push_back(L'\0');
    check(count == expected.size() && std::wstring(text, count) == expected, "unicode paths, NUL separated, double NUL end");
    check((block.size() - header.pFiles) % sizeof(wchar_t) == 0 && text[count - 1] == L'\0' && text[count - 2] == L'\0', "ends with two NULs");

    // Dragging out: the shell's data object for real files answers with exactly those files as CF_HDROP.
    check(filesDataObject({}) == nullptr, "no paths, no data object");
    const auto scratch = std::filesystem::temp_directory_path() / L"refarter drag test é";
    std::error_code ignored;
    std::filesystem::remove_all(scratch, ignored);
    std::filesystem::create_directories(scratch);
    const auto first = scratch / L"apex legends 2026-10-22 20-00-00.mp4", second = scratch / L"clip \U0001F430.mp4";
    for (const auto& file : {first, second}) std::ofstream(file) << "x";
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (IDataObject* data = filesDataObject({first, second, scratch / L"missing.mp4"})) {
        FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM medium{};
        std::vector<std::filesystem::path> dropped;
        if (SUCCEEDED(data->GetData(&format, &medium))) {
            const auto drop = static_cast<HDROP>(medium.hGlobal);
            const UINT files = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < files; ++i) {
                std::wstring name(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
                name.resize(DragQueryFileW(drop, i, name.data(), static_cast<UINT>(name.size())));
                dropped.emplace_back(name);
            }
            ReleaseStgMedium(&medium);
        }
        // The shell may answer with another spelling of the temp folder (8.3 short names, letter case).
        std::error_code same;
        check(dropped.size() == 2 && std::filesystem::equivalent(dropped[0], first, same) && std::filesystem::equivalent(dropped[1], second, same),
              "drag data lists the existing files, missing one left out");
        data->Release();
    } else {
        check(false, "drag data object for real files");
    }
    CoUninitialize();
    std::filesystem::remove_all(scratch, ignored);
    return failures ? 1 : 0;
}
