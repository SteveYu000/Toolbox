#include "hardlink_core.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <limits>
#include <utility>

namespace fs = std::filesystem;

namespace hardlink_remover
{
namespace
{

std::wstring system_error_message(const DWORD error_code)
{
    wchar_t *message_buffer = nullptr;
    const DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error_code,
        0,
        reinterpret_cast<wchar_t *>(&message_buffer),
        0,
        nullptr);

    std::wstring message;
    if (size != 0 && message_buffer != nullptr)
    {
        message.assign(message_buffer, size);
        LocalFree(message_buffer);

        while (!message.empty() && std::iswspace(message.back()) != 0)
        {
            message.pop_back();
        }
    }
    else
    {
        message = L"Unknown Windows error";
    }

    message += L" (" + std::to_wstring(error_code) + L")";
    return message;
}

bool path_less(const fs::path &left, const fs::path &right)
{
    const std::wstring left_text = left.wstring();
    const std::wstring right_text = right.wstring();
    return CompareStringOrdinal(
               left_text.c_str(),
               -1,
               right_text.c_str(),
               -1,
               TRUE) == CSTR_LESS_THAN;
}

bool paths_equal(const fs::path &left, const fs::path &right)
{
    const std::wstring left_text = left.wstring();
    const std::wstring right_text = right.wstring();
    return CompareStringOrdinal(
               left_text.c_str(),
               -1,
               right_text.c_str(),
               -1,
               TRUE) == CSTR_EQUAL;
}

bool make_absolute_path(const fs::path &input, fs::path &output, DWORD &error_code)
{
    const std::wstring input_text = input.wstring();
    if (input_text.empty())
    {
        error_code = ERROR_INVALID_NAME;
        return false;
    }

    DWORD buffer_size = GetFullPathNameW(input_text.c_str(), 0, nullptr, nullptr);
    if (buffer_size == 0)
    {
        error_code = GetLastError();
        return false;
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(buffer_size) + 1U);
    const DWORD written = GetFullPathNameW(
        input_text.c_str(),
        static_cast<DWORD>(buffer.size()),
        buffer.data(),
        nullptr);
    if (written == 0 || written >= buffer.size())
    {
        error_code = written == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
        return false;
    }

    output = fs::path(std::wstring(buffer.data(), written)).lexically_normal();
    error_code = ERROR_SUCCESS;
    return true;
}

bool get_volume_root(const fs::path &absolute_path, fs::path &volume_root, DWORD &error_code)
{
    std::vector<wchar_t> buffer(32768U, L'\0');
    if (GetVolumePathNameW(
            absolute_path.c_str(),
            buffer.data(),
            static_cast<DWORD>(buffer.size())) == FALSE)
    {
        error_code = GetLastError();
        return false;
    }

    volume_root = fs::path(buffer.data());
    error_code = ERROR_SUCCESS;
    return true;
}

fs::path make_link_path(const fs::path &volume_root, const wchar_t *relative_name)
{
    std::wstring relative(relative_name == nullptr ? L"" : relative_name);
    const auto first_non_separator = relative.find_first_not_of(L"\\/");
    if (first_non_separator == std::wstring::npos)
    {
        relative.clear();
    }
    else if (first_non_separator != 0)
    {
        relative.erase(0, first_non_separator);
    }

    return (volume_root / fs::path(relative)).lexically_normal();
}

void set_query_error(LinkQueryResult &result, const DWORD error_code, const std::wstring &context)
{
    result.error_code = error_code;
    result.error_message = context + L": " + system_error_message(error_code);
}

void set_delete_error(DeleteResult &result, const DWORD error_code, const std::wstring &context)
{
    result.error_code = error_code;
    result.error_message = context + L": " + system_error_message(error_code);
}

} // namespace

LinkQueryResult find_hard_links(const fs::path &file_path)
{
    LinkQueryResult result;
    result.requested_path = file_path;

    DWORD error_code = ERROR_SUCCESS;
    fs::path absolute_path;
    if (!make_absolute_path(file_path, absolute_path, error_code))
    {
        set_query_error(result, error_code, L"Unable to resolve the path");
        return result;
    }
    result.requested_path = absolute_path;

    const DWORD attributes = GetFileAttributesW(absolute_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        set_query_error(result, GetLastError(), L"Unable to access the file");
        return result;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        set_query_error(result, ERROR_DIRECTORY, L"The path is a directory, not a file");
        return result;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        set_query_error(result, ERROR_REPARSE_TAG_INVALID, L"Symbolic links and other reparse points are not supported");
        return result;
    }

    fs::path volume_root;
    if (!get_volume_root(absolute_path, volume_root, error_code))
    {
        set_query_error(result, error_code, L"Unable to determine the volume root");
        return result;
    }

    DWORD name_buffer_length = 512;
    std::vector<wchar_t> name_buffer(name_buffer_length, L'\0');
    HANDLE search_handle = INVALID_HANDLE_VALUE;

    for (;;)
    {
        DWORD supplied_length = static_cast<DWORD>(name_buffer.size());
        search_handle = FindFirstFileNameW(
            absolute_path.c_str(),
            0,
            &supplied_length,
            name_buffer.data());
        if (search_handle != INVALID_HANDLE_VALUE)
        {
            break;
        }

        error_code = GetLastError();
        if (error_code != ERROR_MORE_DATA)
        {
            set_query_error(result, error_code, L"Unable to enumerate hard links");
            return result;
        }

        name_buffer.resize(static_cast<std::size_t>(supplied_length) + 1U);
    }

    result.links.push_back(make_link_path(volume_root, name_buffer.data()));

    for (;;)
    {
        DWORD supplied_length = static_cast<DWORD>(name_buffer.size());
        if (FindNextFileNameW(search_handle, &supplied_length, name_buffer.data()) != FALSE)
        {
            result.links.push_back(make_link_path(volume_root, name_buffer.data()));
            continue;
        }

        error_code = GetLastError();
        if (error_code == ERROR_MORE_DATA)
        {
            name_buffer.resize(static_cast<std::size_t>(supplied_length) + 1U);
            continue;
        }
        if (error_code == ERROR_HANDLE_EOF)
        {
            break;
        }

        FindClose(search_handle);
        result.links.clear();
        set_query_error(result, error_code, L"Unable to continue enumerating hard links");
        return result;
    }

    FindClose(search_handle);

    std::sort(result.links.begin(), result.links.end(), path_less);
    result.links.erase(
        std::unique(result.links.begin(), result.links.end(), paths_equal),
        result.links.end());
    return result;
}

std::vector<DeleteResult> delete_hard_links(const std::vector<fs::path> &paths)
{
    struct PlannedDelete
    {
        std::size_t result_index = 0;
        bool valid = false;
    };

    std::vector<DeleteResult> results;
    std::vector<PlannedDelete> plan;
    results.reserve(paths.size());
    plan.reserve(paths.size());

    for (const fs::path &input_path : paths)
    {
        fs::path absolute_path;
        DWORD error_code = ERROR_SUCCESS;
        if (!make_absolute_path(input_path, absolute_path, error_code))
        {
            DeleteResult result;
            result.path = input_path;
            set_delete_error(result, error_code, L"Unable to resolve the path");
            results.push_back(std::move(result));
            plan.push_back({results.size() - 1U, false});
            continue;
        }

        const bool duplicate = std::any_of(
            results.begin(),
            results.end(),
            [&absolute_path](const DeleteResult &existing)
            {
                return paths_equal(existing.path, absolute_path);
            });
        if (duplicate)
        {
            continue;
        }

        DeleteResult result;
        result.path = absolute_path;

        const DWORD attributes = GetFileAttributesW(absolute_path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            set_delete_error(result, GetLastError(), L"Unable to access the file");
            results.push_back(std::move(result));
            plan.push_back({results.size() - 1U, false});
            continue;
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            set_delete_error(result, ERROR_DIRECTORY, L"Refusing to delete a directory");
            results.push_back(std::move(result));
            plan.push_back({results.size() - 1U, false});
            continue;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            set_delete_error(result, ERROR_REPARSE_TAG_INVALID, L"Refusing to delete a reparse point");
            results.push_back(std::move(result));
            plan.push_back({results.size() - 1U, false});
            continue;
        }

        const HANDLE file_handle = CreateFileW(
            absolute_path.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file_handle == INVALID_HANDLE_VALUE)
        {
            set_delete_error(result, GetLastError(), L"Unable to inspect the file");
            results.push_back(std::move(result));
            plan.push_back({results.size() - 1U, false});
            continue;
        }

        BY_HANDLE_FILE_INFORMATION information{};
        const BOOL information_ok = GetFileInformationByHandle(file_handle, &information);
        error_code = information_ok != FALSE ? ERROR_SUCCESS : GetLastError();
        CloseHandle(file_handle);

        if (information_ok == FALSE)
        {
            set_delete_error(result, error_code, L"Unable to read hard-link information");
            results.push_back(std::move(result));
            plan.push_back({results.size() - 1U, false});
            continue;
        }
        if (information.nNumberOfLinks < 2)
        {
            set_delete_error(result, ERROR_INVALID_DATA, L"Refusing to delete a file that no longer has multiple hard links");
            results.push_back(std::move(result));
            plan.push_back({results.size() - 1U, false});
            continue;
        }

        results.push_back(std::move(result));
        plan.push_back({results.size() - 1U, true});
    }

    // 先完成全部校验再开始删除，既允许一次删除同一文件的全部已选名称，
    // 又能保护被误传入的普通单链接文件。
    for (const PlannedDelete &entry : plan)
    {
        if (!entry.valid)
        {
            continue;
        }

        DeleteResult &result = results[entry.result_index];
        if (DeleteFileW(result.path.c_str()) != FALSE)
        {
            result.removed = true;
            continue;
        }

        set_delete_error(result, GetLastError(), L"Unable to delete the hard link");
    }

    return results;
}

} // namespace hardlink_remover
