#include "screenshot.h"

#include <wincodec.h>
#include <winrt/base.h>

namespace refarter::capture {

std::optional<std::wstring> writePng(const std::filesystem::path& path, const std::uint8_t* pixels, UINT width, UINT height, UINT pitch) {
    if (!pixels || !width || !height || pitch < width * 4) return L"the screenshot was empty";
    auto partial = path;
    partial += L".partial";
    HRESULT failed = S_OK;
    try {
        const auto factory = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
        winrt::com_ptr<IWICBitmap> bitmap;
        winrt::check_hresult(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGR, pitch, pitch * height,
            const_cast<BYTE*>(pixels), bitmap.put()));
        {
            winrt::com_ptr<IWICStream> stream;
            winrt::check_hresult(factory->CreateStream(stream.put()));
            winrt::check_hresult(stream->InitializeFromFilename(partial.c_str(), GENERIC_WRITE));
            winrt::com_ptr<IWICBitmapEncoder> encoder;
            winrt::check_hresult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
            winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
            winrt::com_ptr<IWICBitmapFrameEncode> frame;
            winrt::check_hresult(encoder->CreateNewFrame(frame.put(), nullptr));
            winrt::check_hresult(frame->Initialize(nullptr));
            winrt::check_hresult(frame->SetSize(width, height));
            WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR; // No alpha: smaller, and nothing see-through.
            winrt::check_hresult(frame->SetPixelFormat(&format));
            winrt::check_hresult(frame->WriteSource(bitmap.get(), nullptr)); // Converts to the frame's format.
            winrt::check_hresult(frame->Commit());
            winrt::check_hresult(encoder->Commit());
        } // The stream closes the file here, before the rename.
        if (!MoveFileExW(partial.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) winrt::throw_last_error();
        return std::nullopt;
    } catch (const winrt::hresult_error& error) {
        failed = error.code();
    } catch (const std::bad_alloc&) {
        failed = E_OUTOFMEMORY;
    }
    DeleteFileW(partial.c_str());
    if (failed == HRESULT_FROM_WIN32(ERROR_DISK_FULL) || failed == HRESULT_FROM_WIN32(ERROR_HANDLE_DISK_FULL) || failed == STG_E_MEDIUMFULL)
        return L"screenshot not saved · disk full";
    if (failed == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) || failed == HRESULT_FROM_WIN32(ERROR_FILE_EXISTS))
        return L"screenshot not saved · a file with its name appeared";
    return L"screenshot not saved · windows could not write it";
}

} // namespace refarter::capture
