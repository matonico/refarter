#include "recycle_bin.h"

#include <shobjidl.h>
#include <shellapi.h>
#include <winrt/base.h>
#include <algorithm>

namespace refarter {

namespace {
// Moves files to the Recycle Bin or not at all. The shell turns a recycle into a permanent delete when the drive has
// no Recycle Bin, the file is larger than the bin's limit, or the path cannot be recycled; with the confirmation
// switched off it would do that without asking. This sink sees every delete before it happens and aborts the one
// that would not be recycled.
struct RecycleOnlySink : winrt::implements<RecycleOnlySink, IFileOperationProgressSink> {
    STDMETHODIMP StartOperations() noexcept override { return S_OK; }
    STDMETHODIMP FinishOperations(HRESULT) noexcept override { return S_OK; }
    STDMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) noexcept override { return S_OK; }
    STDMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) noexcept override { return S_OK; }
    STDMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) noexcept override { return S_OK; }
    STDMETHODIMP PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) noexcept override { return S_OK; }
    STDMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) noexcept override { return S_OK; }
    STDMETHODIMP PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) noexcept override { return S_OK; }
    STDMETHODIMP PreDeleteItem(DWORD flags, IShellItem*) noexcept override {
        return (flags & TSF_DELETE_RECYCLE_IF_POSSIBLE) ? S_OK : E_ABORT;
    }
    STDMETHODIMP PostDeleteItem(DWORD, IShellItem*, HRESULT, IShellItem*) noexcept override { return S_OK; }
    STDMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) noexcept override { return S_OK; }
    STDMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) noexcept override { return S_OK; }
    STDMETHODIMP UpdateProgress(UINT, UINT) noexcept override { return S_OK; }
    STDMETHODIMP ResetTimer() noexcept override { return S_OK; }
    STDMETHODIMP PauseTimer() noexcept override { return S_OK; }
    STDMETHODIMP ResumeTimer() noexcept override { return S_OK; }
};
} // namespace

std::size_t recycleOnly(const std::vector<std::filesystem::path>& paths, HWND owner) {
    std::vector<winrt::com_ptr<IShellItem>> items;
    for (const auto& path : paths) {
        winrt::com_ptr<IShellItem> item;
        if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.put())))) return paths.size();
        items.push_back(std::move(item));
    }
    winrt::com_ptr<IFileOperation> operation;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(operation.put()));
    if (SUCCEEDED(hr) && owner) hr = operation->SetOwnerWindow(owner);
    // No FOF_WANTNUKEWARNING: a question is not a safe answer, so the sink below is what stops a permanent delete.
    if (SUCCEEDED(hr)) hr = operation->SetOperationFlags(FOFX_RECYCLEONDELETE | FOFX_ADDUNDORECORD | FOF_NOERRORUI | FOFX_EARLYFAILURE | FOF_SILENT | FOF_NOCONFIRMATION);
    // Always advised: without the sink nothing is queued, so a delete the shell would not recycle is refused, not run.
    const auto sink = winrt::make<RecycleOnlySink>();
    DWORD cookie = 0;
    if (SUCCEEDED(hr)) hr = operation->Advise(sink.get(), &cookie);
    const bool advised = SUCCEEDED(hr);
    for (std::size_t i = 0; SUCCEEDED(hr) && i < items.size(); ++i) hr = operation->DeleteItem(items[i].get(), nullptr);
    if (SUCCEEDED(hr)) hr = operation->PerformOperations();
    if (advised) operation->Unadvise(cookie);
    BOOL aborted = TRUE;
    if (SUCCEEDED(hr)) hr = operation->GetAnyOperationsAborted(&aborted);
    std::size_t remaining = 0;
    for (const auto& path : paths)
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) ++remaining;
    return FAILED(hr) || aborted ? std::max<std::size_t>(remaining, 1) : remaining;
}

} // namespace refarter
