#include "source_names.h"
#include <iostream>

using namespace refarter;

int main() {
    if (friendlySourceName(L"  Apex Legends  ", L"r5apex_dx12.exe") != L"apex legends" ||
        friendlySourceName(L" \t", L"Example.exe") != L"example.exe") return 1;
    const auto labels = sourceLabels({{L"Apex Legends", L"r5apex_dx12.exe"},
        {L"editor", L"first.exe"}, {L"EDITOR", L"second.exe"},
        {L"refarter", L"refarter.exe"}, {L"refarter", L"refarter.exe"}});
    const std::vector<std::wstring> expected{L"apex legends", L"editor · first.exe", L"editor · second.exe",
        L"refarter · refarter.exe · window 1", L"refarter · refarter.exe · window 2"};
    if (labels != expected) { std::cerr << "friendly labels do not uniquely identify duplicate windows\n"; return 1; }
    if (sourceLabels({{L"",L"game.exe"},{L"",L"game.exe"}}) !=
        std::vector<std::wstring>{L"game.exe · window 1",L"game.exe · window 2"}) return 1;

    // A whole-monitor save takes the listed game's name only while it is in front on the recorded monitor.
    int failures = 0;
    const auto check = [&](bool passes, const char* name) {
        if (!passes) { std::cerr << name << '\n'; ++failures; }
    };
    const ForegroundApp deadlock{true, true, L"  Deadlock ", L"project8.exe"};
    check(monitorSaveName(L"Monitor 1", deadlock) == L"deadlock", "a listed game in front names the clip like its window capture");
    ForegroundApp untitled = deadlock;
    untitled.title.clear();
    check(monitorSaveName(L"Monitor 1", untitled) == L"project8.exe", "an untitled listed game is named after its executable");
    ForegroundApp unlisted = deadlock;
    unlisted.listed = false;
    check(monitorSaveName(L"Monitor 1", unlisted) == L"monitor 1", "an unlisted app in front keeps the monitor name");
    ForegroundApp elsewhere = deadlock;
    elsewhere.onMonitor = false;
    check(monitorSaveName(L"Monitor 1", elsewhere) == L"monitor 1", "a listed game on another monitor keeps the monitor name");
    check(monitorSaveName(L"Monitor 1", std::nullopt) == L"monitor 1", "no usable window in front keeps the monitor name");

    // A game is added under its window title up to the first separator, without version, build or platform tags.
    const auto named = [&](const wchar_t* title, const wchar_t* expected, const char* name) {
        if (const auto got = gameName(title, L"C:\\Games\\game.exe"); got != expected) {
            // Non-ASCII would put std::wcerr in a failed state; print it as code points instead.
            for (const wchar_t c : got) if (c < 128) std::cerr << static_cast<char>(c); else std::cerr << "\\u" << std::hex << int(c) << std::dec;
            std::cerr << '\n';
            check(false, name);
        }
        // The shorten offer runs gameName on saved names; a name that is already short must come out unchanged.
        check(gameName(expected, L"C:\\Games\\game.exe") == expected, "a short name stays as it is when shortened again");
    };
    named(L"Stardew Valley 1.6.15 - running SMAPI 4.1.10 with 3 mods", L"stardew valley", "a version and a mod loader's text are left out");
    named(L"lichii | build-116 | The Legend of Zelda", L"lichii", "an emulator is named after itself, not the game it runs");
    named(L"Apex Legends", L"apex legends", "a plain title stays whole");
    named(L"Battlefield 4", L"battlefield 4", "a plain number is part of the name");
    named(L"Doom (1993)", L"doom (1993)", "a bracketed year is part of the name");
    named(L"Half-Life 2", L"half-life 2", "a hyphen inside a word is not a separator");
    named(L"Minecraft 1.21.4 - Multiplayer (3rd-party Server)", L"minecraft", "the version before a separator is left out");
    named(L"Game v2.0.1 (64 bit) [DX12]", L"game", "several tags at the end are left out");
    named(L"Factorio build 116", L"factorio", "a spaced build number is left out");
    named(L"Rebuild 116", L"rebuild 116", "a word ending in build is not a build tag");
    named(L"Hades II — Early Access", L"hades ii — early access", "a dash alone does not cut");
    named(L"Batman – Arkham Knight", L"batman – arkham knight", "a subtitle after a dash stays");
    named(L"S.T.A.L.K.E.R. 2 - Heart of Chornobyl", L"s.t.a.l.k.e.r. 2 - heart of chornobyl", "a plain number before a dash does not cut");
    named(L"Stardew Valley 1.6.15 running SMAPI", L"stardew valley", "a version cuts without a separator");
    named(L"Game - DX12 renderer", L"game - dx12 renderer", "a platform word in the middle does not cut");
    named(L"V8 Supercars", L"v8 supercars", "a version-like first word stays");
    named(L"Game (64-bit, DX11)", L"game", "a bracket of several tags is left out");
    named(L"Game (Early Access, v2)", L"game (early access, v2)", "a bracket with words besides tags stays");
    named(L"Minecraft · javaw.exe · window 2", L"minecraft", "what the picker added to an old label is left out");
    named(L"  1.0.2 ", L"game", "a title that is only a tag falls back to the executable");
    named(L"| loading", L"game", "nothing before the separator falls back to the executable");
    check(gameName(L"", L"C:\\Games\\Example.EXE") == L"example", "an untitled game is named after its executable");
    check(gameName(L"", L"") == L"", "nothing to name a game after gives no name");

    // The picker's additions survive a game's short name in the footer.
    check(pickerSuffix(L"minecraft 1.21 · javaw.exe · window 2", L"javaw.exe") == L" · javaw.exe · window 2",
          "the picker's additions are found after the title");
    check(pickerSuffix(L"minecraft 1.21", L"javaw.exe").empty(), "a plain label has no additions");
    check(pickerSuffix(L"stardew valley", L"sv.exe").empty(), "a list name is not mistaken for additions");
    check(pickerSuffix(L"game.exe · window 2", L"game.exe") == L" · window 2", "an untitled window keeps only its number");
    check(pickerSuffix(L"javaw.exe", L"javaw.exe").empty(), "an executable alone is the name, not an addition");

    // Shown names are cut with an ellipsis; short ones and stored ones stay whole.
    check(shownName(L"apex legends") == L"apex legends", "a short name is shown whole");
    const std::wstring longTitle(60, L'a');
    const auto cut = shownName(longTitle);
    check(cut.size() == 48 && cut.ends_with(L"…"), "a long name is cut to the limit with an ellipsis");
    check(shownName(L"abcd efgh", 6) == L"abcd…", "a cut does not end in a space");
    check(shownName(std::wstring(L"abcd") + L"\xd83d\xde00" + L"xyz", 6) == L"abcd…", "a cut never splits a surrogate pair");
    const auto longLabels = sourceLabels({{longTitle + L" one", L"first.exe"}, {longTitle + L" two", L"second.exe"}});
    check(longLabels[0] != longLabels[1] && longLabels[0].starts_with(shownName(longTitle + L" one")),
          "long titles cut alike are still told apart in the picker");

    // Independent tester: more title shapes from the naming rule.
    named(L"Halo · Infinite", L"halo", "a spaced middle dot cuts");
    named(L"Halo·Infinite", L"halo·infinite", "a middle dot without spaces does not cut");
    named(L"Game 1.2 | Foo", L"game", "a version before the bar cuts first");
    named(L"Foo Bar | 1.0", L"foo bar", "a bar before the version cuts first");
    named(L"Foo · Bar 1.0", L"foo", "a middle dot before the version cuts first");
    named(L"Game v2 Remastered", L"game", "a v-number cuts");
    named(L"Game V2.0.1 Beta", L"game", "a dotted v-version cuts, whatever its capitals");
    named(L"Game build-116 nightly", L"game", "a hyphenated build cuts");
    named(L"Game build 116 nightly", L"game", "a spaced build cuts in the middle");
    named(L"Lichii [build-116] Zelda", L"lichii", "a bracketed build cuts");
    named(L"Game (64-bit)", L"game", "a bracketed bitness at the end is left out");
    named(L"Game [DX12]", L"game", "a bracketed renderer at the end is left out");
    named(L"Game x64", L"game", "x64 at the end is left out");
    named(L"Game Vulkan", L"game", "vulkan at the end is left out");
    named(L"Game - Vulkan", L"game", "a platform tag after a dash is left out with the dash");
    named(L"Game x64 1.0", L"game", "a platform tag left before a version is left out");
    named(L"Batman: Arkham Knight (DX11)", L"batman: arkham knight", "a colon in the name stays");
    named(L"FIFA 23", L"fifa 23", "a plain number after the name stays");
    named(L"V8", L"v8", "a version-like first word stays even alone");
    named(L"KINGDOM HEARTS HD 2.8 Final Chapter Prologue", L"kingdom hearts hd 2.8 final chapter prologue",
          "a two-part number before a word is part of the name");
    named(L"Kingdom Hearts HD 1.5 + 2.5 ReMIX", L"kingdom hearts hd 1.5 + 2.5 remix", "a two-part number before \"+\" stays");
    named(L"Stardew Valley 1.6 - running SMAPI", L"stardew valley", "a two-part version before a dash cuts");
    named(L"Stardew Valley 1.6", L"stardew valley", "a two-part version at the end is left out");
    named(L"Stardew Valley 1.6 running SMAPI", L"stardew valley 1.6 running smapi", "a two-part number before a word does not cut");
    named(L"1.6.15 Stardew", L"1.6.15 stardew", "a version as the first word does not cut");
    named(L"   \t ", L"game", "a blank title falls back to the executable");
    check(gameName(L"", L"D:/Games/Foo.Exe") == L"foo", "a forward-slash path gives the file name");
    check(gameName(L"| x", L"Foo.EXE") == L"foo", "an executable given alone loses .exe");
    check(gameName(L"", L"C:\\Games\\launcher") == L"launcher", "an executable without .exe keeps its name");
    {
        // Applying the rule to its own result changes nothing, for odd titles too.
        for (const wchar_t* title : {L"Game (64-bit", L"Game [a (x64)]", L"Game (Early Access) (64-bit)", L"Foo – 2.0 – Bar",
                                     L"Game: v2", L"v2 v3", L"1.0 2.0", L"Build 116", L"Game-build 116", L"((x64))",
                                     L"Game | | |", L"a · b · c", L"Doom(1993) x64", L"Game )x64(", L"Game build"}) {
            const auto once = gameName(title, L"C:\\Games\\game.exe");
            if (gameName(once, L"C:\\Games\\game.exe") != once) { std::wcerr << title << L" -> " << once << L'\n'; check(false, "the rule applied twice changes nothing"); }
        }
    }
    {
        // A stored name is never cut, however long.
        const std::wstring longName(100, L'z');
        check(gameName(longName, L"C:\\Games\\game.exe") == longName, "a long stored name is not cut");
    }

    // Shown names: 48 UTF-16 units, a single "…" unit, never a split pair or a space before the "…".
    {
        const std::wstring exact(48, L'a'), over(49, L'a');
        check(shownName(exact) == exact, "a 48-character name is shown whole");
        check(shownName(over) == std::wstring(47, L'a') + L"…", "a 49-character name is cut to 47 and an ellipsis");
        const auto spaced = shownName(std::wstring(46, L'a') + L" bbbbbbb");
        check(spaced == std::wstring(46, L'a') + L"…", "a cut at a space drops the space");
        const auto spaces = shownName(std::wstring(40, L'a') + std::wstring(20, L' ') + L"b");
        check(spaces == std::wstring(40, L'a') + L"…", "several spaces before the cut are dropped");
        const auto pair = shownName(std::wstring(46, L'a') + L"\xd83d\xde00" + L"bbbb");
        check(pair == std::wstring(46, L'a') + L"…", "a surrogate pair at the default limit is not split");
        const auto wholePair = shownName(std::wstring(45, L'a') + L"\xd83d\xde00" + L"bbbb");
        check(wholePair == std::wstring(45, L'a') + L"\xd83d\xde00" + L"…", "a whole pair before the cut is kept");
        for (const auto& shown : {spaced, spaces, pair, wholePair, shownName(over)})
            check(shown.size() <= 48 && !shown.ends_with(L" …"), "a shown name stays within 48 with no space before the ellipsis");
    }

    // Picker labels for windows whose cut titles are alike.
    {
        const std::wstring longTitle2(60, L'b');
        const std::vector<SourceName> windows{{longTitle2 + L" one", L"game.exe"}, {longTitle2 + L" two", L"game.exe"},
                                              {longTitle2 + L" three", L"other.exe"}, {L"Short", L"game.exe"}};
        const auto shown = sourceLabels(windows);
        const auto stem = shownName(lower(longTitle2 + L" one"));
        check(shown[0] == stem + L" · game.exe · window 1" && shown[1] == stem + L" · game.exe · window 2" &&
              shown[2] == stem + L" · other.exe" && shown[3] == L"short",
              "cut-alike titles get the executable and, for the same executable, a window number");
        for (std::size_t i = 0; i < shown.size(); ++i)
            for (std::size_t j = i + 1; j < shown.size(); ++j) check(shown[i] != shown[j], "every picker label is distinct");
        check(pickerSuffix(shown[1], windows[1].executable) == L" · game.exe · window 2",
              "the picker's additions are found after a cut title");
        check(pickerSuffix(shown[2], windows[2].executable) == L" · other.exe",
              "an executable-only addition is found after a cut title");
        check(pickerSuffix(shown[3], windows[3].executable).empty(), "a unique window has no additions");
        const auto caps = sourceLabels({{L"Editor", L"same.exe"}, {L"EDITOR", L"same.exe"}});
        check(caps == std::vector<std::wstring>{L"editor · same.exe · window 1", L"editor · same.exe · window 2"},
              "titles differing in capitals from one executable are numbered");
        // The window's title changed after the picker listed it: its executable and number still tell it apart.
        check(pickerSuffix(L"minecraft · javaw.exe · window 2", L"javaw.exe") == L" · javaw.exe · window 2",
              "a label made for an older title keeps its additions");
    }

    // Distinct titles that shorten alike (#167 review): the picker numbers them, so the shown source keeps them apart.
    {
        const std::vector<SourceName> windows{{L"Minecraft 1.21.4 - Server A", L"javaw.exe", L"minecraft"},
                                              {L"Minecraft 1.21.4 - Server B", L"javaw.exe", L"minecraft"},
                                              {L"Minecraft Launcher", L"launcher.exe", L"minecraft launcher"},
                                              {L"Notes", L"javaw.exe", L"notes"}};
        const auto alike = sourceLabels(windows);
        check(alike[0] == L"minecraft 1.21.4 - server a · javaw.exe · window 1" &&
              alike[1] == L"minecraft 1.21.4 - server b · javaw.exe · window 2",
              "titles that shorten alike get the executable and a window number");
        check(alike[2] == L"minecraft launcher" && alike[3] == L"notes", "windows whose short names differ get nothing");
        check(pickerSuffix(alike[0], L"javaw.exe") != pickerSuffix(alike[1], L"javaw.exe"),
              "their additions differ, so the shown sources differ");
        const auto otherExe = sourceLabels({{L"Game 1.2 - Alpha", L"a.exe", L"game"}, {L"Game 3.4 - Beta", L"b.exe", L"game"}});
        check(otherExe == std::vector<std::wstring>{L"game 1.2 - alpha · a.exe", L"game 3.4 - beta · b.exe"},
              "alike short names from different executables get only the executable");
    }
    return failures ? 1 : 0;
}
