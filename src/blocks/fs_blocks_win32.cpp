// Win32 Shell New for fsNew — localized "New <type>.<ext>" via IFileOperation.

#include "blocks/fs_blocks.hpp"
#include "fs/path_sanitizer.hpp"

#if defined(_WIN32)

#include "process/process_helpers.hpp"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace media::xblox::blocks {
namespace {

using Microsoft::WRL::ComPtr;

std::wstring normalize_extension(std::string ext)
{
    while (!ext.empty() && std::isspace(static_cast<unsigned char>(ext.front())))
        ext.erase(ext.begin());
    while (!ext.empty() && (ext.front() == '*' || ext.front() == '.'))
        ext.erase(ext.begin());
    while (!ext.empty() && std::isspace(static_cast<unsigned char>(ext.back())))
        ext.pop_back();
    if (ext.empty())
        return {};
    std::wstring wide = media::process::widen_utf8(ext);
    for (wchar_t& ch : wide)
        ch = static_cast<wchar_t>(towlower(ch));
    return L"." + wide;
}

std::wstring reg_sz(HKEY root, const std::wstring& sub, const wchar_t* value)
{
    wchar_t buf[1024]{};
    DWORD size = sizeof(buf);
    if (RegGetValueW(
            root,
            sub.c_str(),
            value,
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            nullptr,
            buf,
            &size)
        != ERROR_SUCCESS) {
        return {};
    }
    return buf;
}

bool reg_key_exists(HKEY root, const std::wstring& sub)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    RegCloseKey(key);
    return true;
}

std::wstring shell_new_key(const std::wstring& ext)
{
    const std::wstring direct = ext + L"\\ShellNew";
    if (reg_key_exists(HKEY_CLASSES_ROOT, direct))
        return direct;
    const std::wstring progid = reg_sz(HKEY_CLASSES_ROOT, ext, nullptr);
    if (progid.empty())
        return {};
    const std::wstring nested = ext + L"\\" + progid + L"\\ShellNew";
    if (reg_key_exists(HKEY_CLASSES_ROOT, nested))
        return nested;
    return {};
}

std::wstring load_indirect(const std::wstring& value)
{
    if (value.empty())
        return {};
    wchar_t buf[512]{};
    if (SUCCEEDED(SHLoadIndirectString(
            value.c_str(), buf, static_cast<UINT>(std::size(buf)), nullptr))
        && buf[0]) {
        return buf;
    }
    return value.front() == L'@' ? std::wstring{} : value;
}

std::wstring friendly_type_name(const std::wstring& ext)
{
    wchar_t name[256]{};
    DWORD cch = static_cast<DWORD>(std::size(name));
    if (SUCCEEDED(AssocQueryStringW(
            ASSOCF_NONE,
            ASSOCSTR_FRIENDLYDOCNAME,
            ext.c_str(),
            nullptr,
            name,
            &cch))
        && name[0]) {
        return name;
    }
    SHFILEINFOW info{};
    if (SHGetFileInfoW(
            ext.c_str(),
            FILE_ATTRIBUTE_NORMAL,
            &info,
            sizeof(info),
            SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES)
        && info.szTypeName[0]) {
        return info.szTypeName;
    }
    return ext.size() > 1 ? ext.substr(1) : ext;
}

std::wstring new_name_format()
{
    HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
    if (!shell32)
        shell32 = LoadLibraryW(L"shell32.dll");
    wchar_t buf[128]{};
    // shell32 IDS 30316 = localized "New %s" (Win10/11).
    if (shell32
        && LoadStringW(shell32, 30316, buf, 128) > 0
        && wcsstr(buf, L"%s")) {
        return buf;
    }
    return L"New %s";
}

bool ends_with_ci(const std::wstring& value, const std::wstring& suffix)
{
    if (value.size() < suffix.size())
        return false;
    return _wcsicmp(value.c_str() + (value.size() - suffix.size()), suffix.c_str())
        == 0;
}

std::wstring with_extension(std::wstring name, const std::wstring& ext)
{
    if (ends_with_ci(name, ext))
        return name;
    return name + ext;
}

std::wstring explorer_new_filename(
    const std::wstring& ext,
    const std::wstring& name_override)
{
    if (!name_override.empty())
        return with_extension(name_override, ext);

    const std::wstring key = shell_new_key(ext);
    const std::wstring item = key.empty()
        ? std::wstring{}
        : load_indirect(reg_sz(HKEY_CLASSES_ROOT, key, L"ItemName"));
    const std::wstring type = item.empty() ? friendly_type_name(ext) : item;
    const std::wstring format = new_name_format();
    const std::wstring prefix = format.substr(0, format.find(L"%s"));
    if (!item.empty() && (ends_with_ci(item, ext) || (!prefix.empty() && item.rfind(prefix, 0) == 0)))
        return with_extension(item, ext);

    const auto insert = format.find(L"%s");
    const std::wstring formatted = insert == std::wstring::npos
        ? format + type
        : format.substr(0, insert) + type + format.substr(insert + 2);
    return with_extension(formatted, ext);
}

std::wstring unique_new_path(
    const std::filesystem::path& folder,
    const std::wstring& filename)
{
    // PathYetAnotherMakeUniqueName treats pszPath as a *file* path. Passing
    // only the folder makes it park the file in the parent (xblox → shared\).
    std::filesystem::path dest = folder / filename;
    std::error_code ec;
    if (!std::filesystem::exists(dest, ec))
        return dest.wstring();
    const std::wstring stem = dest.stem().wstring();
    const std::wstring dest_ext = dest.extension().wstring();
    for (int n = 2; n < 10000; ++n) {
        auto candidate = folder
            / (stem + L" (" + std::to_wstring(n) + L")" + dest_ext);
        if (!std::filesystem::exists(candidate, ec))
            return candidate.wstring();
    }
    return {};
}

std::vector<unsigned char> shell_new_data(const std::wstring& key)
{
    if (key.empty())
        return {};
    DWORD size = 0;
    if (RegGetValueW(
            HKEY_CLASSES_ROOT,
            key.c_str(),
            L"Data",
            RRF_RT_REG_BINARY,
            nullptr,
            nullptr,
            &size)
        != ERROR_SUCCESS
        || size == 0) {
        return {};
    }
    std::vector<unsigned char> data(size);
    if (RegGetValueW(
            HKEY_CLASSES_ROOT,
            key.c_str(),
            L"Data",
            RRF_RT_REG_BINARY,
            nullptr,
            data.data(),
            &size)
        != ERROR_SUCCESS) {
        return {};
    }
    data.resize(size);
    return data;
}

bool create_empty_file(const std::wstring& full_path, std::string& err)
{
    HANDLE file = CreateFileW(
        full_path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        err = "CreateFile failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(file);
    return true;
}

bool create_shell_new_file(
    const std::wstring& folder,
    const std::wstring& full_path,
    const std::wstring& template_name,
    std::string& err)
{
    if (!template_name.empty() && PathFileExistsW(template_name.c_str())) {
        if (!CopyFileW(template_name.c_str(), full_path.c_str(), TRUE)) {
            err = "CopyFile failed (" + std::to_string(GetLastError()) + ")";
            return false;
        }
        return true;
    }

    struct ComScope {
        bool uninit = false;
        HRESULT status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ComScope()
        {
            uninit = status == S_OK;
        }
        ~ComScope()
        {
            if (uninit)
                CoUninitialize();
        }
        bool sta() const
        {
            return status == S_OK || status == S_FALSE;
        }
    } com;

    // IFileOperation is STA-only and silently no-ops for types the Shell
    // does not know (.xblox, etc.). Unknown extensions do not need a
    // ProgID / ShellNew key — we just write an empty file.
    if (com.sta()) {
        const wchar_t* name_only = PathFindFileNameW(full_path.c_str());
        if (name_only && name_only[0]) {
            ComPtr<IFileOperation> operation;
            HRESULT hr = CoCreateInstance(
                CLSID_FileOperation,
                nullptr,
                CLSCTX_ALL,
                IID_PPV_ARGS(&operation));
            if (SUCCEEDED(hr)) {
                (void)operation->SetOwnerWindow(nullptr);
                (void)operation->SetOperationFlags(
                    FOF_SILENT | FOF_NOCONFIRMATION | FOF_NOERRORUI
                    | FOF_NOCONFIRMMKDIR);
                ComPtr<IShellItem> folder_item;
                hr = SHCreateItemFromParsingName(
                    folder.c_str(), nullptr, IID_PPV_ARGS(&folder_item));
                const wchar_t* tmpl =
                    template_name.empty() ? nullptr : template_name.c_str();
                if (SUCCEEDED(hr)
                    && SUCCEEDED(operation->NewItem(
                        folder_item.Get(),
                        FILE_ATTRIBUTE_NORMAL,
                        name_only,
                        tmpl,
                        nullptr))) {
                    (void)operation->PerformOperations();
                }
            }
        }
    }

    if (PathFileExistsW(full_path.c_str()))
        return true;
    return create_empty_file(full_path, err);
}

} // namespace

bool fs_new_block(BlockIO& io)
{
    std::string ext_utf8 = io.str("ext");
    if (ext_utf8.empty())
        ext_utf8 = io.str("extension", "txt");
    const std::wstring ext = normalize_extension(ext_utf8);
    if (ext.size() < 2) {
        io.error("fsNew", "fsNew: extension is empty", 2);
        return true;
    }

    std::string folder_utf8 = io.str("path");
    // setVariable valueFrom only sees xblox context, not CWD/CURRENT_PATH.
    // A missed lookup stores JSON null, and ${folder} becomes the literal "null".
    if (folder_utf8 == "null" || folder_utf8 == "undefined"
        || (folder_utf8.size() >= 3 && folder_utf8.front() == '$'
            && folder_utf8[1] == '{')) {
        folder_utf8.clear();
    }
    std::error_code ec;
    std::filesystem::path folder = folder_utf8.empty()
        ? std::filesystem::current_path(ec)
        : std::filesystem::path(media::process::widen_utf8(folder_utf8));
    if (ec) {
        io.error("fsNew", "fsNew: cannot resolve current directory", 1);
        return true;
    }
    if (std::filesystem::is_regular_file(folder, ec))
        folder = folder.parent_path();
    if (!std::filesystem::is_directory(folder, ec)) {
        std::filesystem::create_directories(folder, ec);
        if (ec) {
            io.error("fsNew", "fsNew: cannot create folder: " + ec.message(), 1);
            return true;
        }
    }
    folder = std::filesystem::absolute(folder, ec);
    if (ec) {
        io.error("fsNew", "fsNew: cannot resolve folder: " + ec.message(), 1);
        return true;
    }

    const std::wstring name_override =
        media::process::widen_utf8(io.str("name"));
    const std::wstring filename = explorer_new_filename(ext, name_override);
    const std::wstring full_w = unique_new_path(folder, filename);
    if (full_w.empty()) {
        io.error("fsNew", "fsNew: could not allocate a unique name", 1);
        return true;
    }

    const std::wstring key = shell_new_key(ext);
    const std::wstring template_name = key.empty()
        ? std::wstring{}
        : reg_sz(HKEY_CLASSES_ROOT, key, L"FileName");
    std::string err;
    if (!create_shell_new_file(folder.wstring(), full_w, template_name, err)) {
        io.error("fsNew", "fsNew: " + err, 1);
        return true;
    }

    const auto payload = shell_new_data(key);
    if (!payload.empty()) {
        std::ofstream out(std::filesystem::path(full_w), std::ios::binary | std::ios::trunc);
        if (!out) {
            io.error("fsNew", "fsNew: cannot write ShellNew data", 1);
            return true;
        }
        out.write(
            reinterpret_cast<const char*>(payload.data()),
            static_cast<std::streamsize>(payload.size()));
    }

    if (!std::filesystem::exists(std::filesystem::path(full_w), ec)) {
        io.error("fsNew", "fsNew: file was not created", 1);
        return true;
    }

    const std::string abs_path = media::process::narrow_utf8(
        std::filesystem::absolute(full_w, ec).wstring());
    const std::string ext_narrow = media::process::narrow_utf8(ext);
    io.ok("fsNew", "created: " + abs_path)
      .set("path", abs_path)
      .set("ext", ext_narrow)
      .set("extension", ext_narrow)
      .result(abs_path, ParamKind::file_path);
    return true;
}

} // namespace media::xblox::blocks

#else // !_WIN32

namespace media::xblox::blocks {

bool fs_new_block(BlockIO& io)
{
    io.error("fsNew", "fsNew: Windows Shell New is only available on Win32", 2);
    return true;
}

} // namespace media::xblox::blocks

#endif // _WIN32
