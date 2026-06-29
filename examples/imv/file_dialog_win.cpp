// Windows native file dialog
#include "file_dialog.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#include <ShlObj.h>

namespace imv {

bool OpenVideoFiles(std::vector<std::string>& out_paths) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return false;

    IFileOpenDialog* pOpen = nullptr;
    hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                          IID_IFileOpenDialog, (void**)&pOpen);
    if (FAILED(hr)) { CoUninitialize(); return false; }

    DWORD flags;
    pOpen->GetOptions(&flags);
    pOpen->SetOptions(flags | FOS_FILEMUSTEXIST);
    pOpen->SetTitle(L"Open Video File");

    hr = pOpen->Show(nullptr);
    if (SUCCEEDED(hr)) {
        IShellItem* pItem = nullptr;
        if (SUCCEEDED(pOpen->GetResult(&pItem))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
                if (len > 0) {
                    std::string utf8(size_t(len) - 1, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8.data(), len, nullptr, nullptr);
                    out_paths.push_back(std::move(utf8));
                }
                CoTaskMemFree(path);
            }
            pItem->Release();
        }
    }

    pOpen->Release();
    CoUninitialize();
    return !out_paths.empty();
}

} // namespace imv
