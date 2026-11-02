#include "game_icons.h"
#include "accent_colour.h"
#include "local_file.h"
#include "text.h"
#include "theme.h"

#include <shobjidl.h>
#include <winrt/base.h>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <vector>

namespace refarter {
namespace {
constexpr std::uintmax_t maxBytes = 1024 * 1024;
constexpr wchar_t header[] = L"refarter game executables 1";

// FNV-1a over the lower-case path: the same name for the same executable on every run.
std::uint64_t pathHash(const std::filesystem::path& executable) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const wchar_t c : lower(executable.lexically_normal().wstring())) {
        hash ^= static_cast<std::uint16_t>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

// Extracts the icon through the Shell (local only) and writes it as a straight-alpha PNG, replacing an older one.
bool extractIcon(IWICImagingFactory* wic, const std::filesystem::path& executable, int size, const std::filesystem::path& cached) {
    winrt::com_ptr<IShellItemImageFactory> shell;
    if (FAILED(SHCreateItemFromParsingName(executable.c_str(), nullptr, IID_PPV_ARGS(shell.put())))) return false;
    HBITMAP bitmap = nullptr;
    if (FAILED(shell->GetImage({size, size}, SIIGBF_ICONONLY, &bitmap)) || !bitmap) return false;
    winrt::com_ptr<IWICBitmap> source;
    const auto made = wic->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapUsePremultipliedAlpha, source.put());
    DeleteObject(bitmap);
    if (FAILED(made)) return false;
    std::error_code error;
    std::filesystem::create_directories(cached.parent_path(), error);
    const auto partial = std::filesystem::path(cached).concat(L".partial");
    try {
        winrt::com_ptr<IWICFormatConverter> straight;
        winrt::check_hresult(wic->CreateFormatConverter(straight.put()));
        winrt::check_hresult(straight->Initialize(source.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                                  WICBitmapPaletteTypeCustom));
        winrt::com_ptr<IWICStream> stream;
        winrt::check_hresult(wic->CreateStream(stream.put()));
        winrt::check_hresult(stream->InitializeFromFilename(partial.c_str(), GENERIC_WRITE));
        winrt::com_ptr<IWICBitmapEncoder> encoder;
        winrt::check_hresult(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
        winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
        winrt::com_ptr<IWICBitmapFrameEncode> frame;
        winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
        winrt::check_hresult(frame->Initialize(nullptr));
        UINT width = 0, height = 0;
        winrt::check_hresult(straight->GetSize(&width, &height));
        winrt::check_hresult(frame->SetSize(width, height));
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        winrt::check_hresult(frame->SetPixelFormat(&format));
        winrt::check_hresult(frame->WriteSource(straight.get(), nullptr));
        winrt::check_hresult(frame->Commit());
        winrt::check_hresult(encoder->Commit());
    } catch (const winrt::hresult_error&) {
        std::filesystem::remove(partial, error);
        return false;
    }
    if (MoveFileExW(partial.c_str(), cached.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    std::filesystem::remove(partial, error);
    return false;
}

// The cached PNG as a premultiplied top-down DIB, as AlphaBlend wants it.
HBITMAP loadIcon(IWICImagingFactory* wic, const std::filesystem::path& cached) {
    try {
        winrt::com_ptr<IWICBitmapDecoder> decoder;
        winrt::check_hresult(wic->CreateDecoderFromFilename(cached.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put()));
        winrt::com_ptr<IWICBitmapFrameDecode> frame;
        winrt::check_hresult(decoder->GetFrame(0, frame.put()));
        winrt::com_ptr<IWICFormatConverter> premultiplied;
        winrt::check_hresult(wic->CreateFormatConverter(premultiplied.put()));
        winrt::check_hresult(premultiplied->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                                       WICBitmapPaletteTypeCustom));
        UINT width = 0, height = 0;
        winrt::check_hresult(premultiplied->GetSize(&width, &height));
        if (!width || !height || width > 512 || height > 512) return nullptr;
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), static_cast<LONG>(width), -static_cast<LONG>(height), 1, 32, BI_RGB};
        void* bits = nullptr;
        const HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap) return nullptr;
        if (FAILED(premultiplied->CopyPixels(nullptr, width * 4, width * height * 4, static_cast<BYTE*>(bits)))) {
            DeleteObject(bitmap);
            return nullptr;
        }
        return bitmap;
    } catch (const winrt::hresult_error&) {
        return nullptr;
    }
}
} // namespace

std::filesystem::path gameExecutablesPath() { return localAppDataFile(L"games.dat"); }

std::optional<GameExecutables> readGameExecutables(const std::filesystem::path& file) {
    std::wstring text;
    switch (readUtf16File(file, maxBytes, text)) {
    case Utf16Read::Missing: return GameExecutables{};
    case Utf16Read::Ok: break;
    default: return std::nullopt;
    }
    std::wistringstream lines(text);
    std::wstring line;
    if (!std::getline(lines, line) || line != header) return std::nullopt;
    GameExecutables games;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        std::wistringstream record(line);
        std::wstring game, executable;
        if (!(record >> std::quoted(game) >> std::quoted(executable))) return std::nullopt;
        record >> std::ws;
        if (!record.eof() || game.empty() || executable.empty()) return std::nullopt;
        games[lower(game)] = std::move(executable);
    }
    return games;
}

bool writeGameExecutables(const std::filesystem::path& file, const GameExecutables& games) {
    std::wostringstream text;
    text << header << L'\n';
    for (const auto& [game, executable] : games) text << std::quoted(game) << L' ' << std::quoted(executable) << L'\n';
    return writeUtf16FileAtomic(file, text.str(), maxBytes, L".games-");
}

std::filesystem::path gameIconCachePath(const std::filesystem::path& folder, const std::filesystem::path& executable, int size) {
    wchar_t name[48]{};
    swprintf_s(name, L"%016llx-%d.png", static_cast<unsigned long long>(pathHash(executable)), size);
    return folder / name;
}

HBITMAP gameIcon(IWICImagingFactory* wic, const std::filesystem::path& executable, int size, const std::filesystem::path& cacheFolder) {
    if (!wic || size <= 0 || size > 256 || cacheFolder.empty()) return nullptr;
    std::error_code error;
    const auto changed = std::filesystem::last_write_time(executable, error);
    if (error) return nullptr; // Moved or uninstalled: the folder keeps its plain look.
    const auto cached = gameIconCachePath(cacheFolder, executable, size);
    std::error_code missing;
    const auto made = std::filesystem::last_write_time(cached, missing);
    if ((missing || made < changed) && !extractIcon(wic, executable, size, cached)) return nullptr;
    return loadIcon(wic, cached);
}

std::optional<COLORREF> gameIconAccent(HBITMAP icon) {
    DIBSECTION dib{};
    if (!icon || GetObjectW(icon, sizeof(dib), &dib) != sizeof(dib) || !dib.dsBm.bmBits || dib.dsBm.bmBitsPixel != 32) return std::nullopt;
    return iconAccent(static_cast<const std::uint8_t*>(dib.dsBm.bmBits), dib.dsBm.bmWidth, std::abs(dib.dsBm.bmHeight),
                      dib.dsBm.bmWidthBytes, theme::raised);
}

} // namespace refarter
