#include "hardlink_core.h"

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

int failures = 0;

void expect(const bool condition, const char *description)
{
    if (!condition)
    {
        ++failures;
        std::cerr << "FAILED: " << description << '\n';
    }
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

bool contains_path(const std::vector<fs::path> &paths, const fs::path &expected)
{
    for (const fs::path &path : paths)
    {
        if (paths_equal(path, expected))
        {
            return true;
        }
    }
    return false;
}

fs::path absolute_path(const fs::path &path)
{
    std::error_code error;
    const fs::path absolute = fs::absolute(path, error).lexically_normal();
    return error ? path : absolute;
}

class TemporaryDirectory
{
public:
    explicit TemporaryDirectory(fs::path path)
        : path_(std::move(path))
    {
    }

    TemporaryDirectory(const TemporaryDirectory &) = delete;
    TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    [[nodiscard]] const fs::path &path() const noexcept
    {
        return path_;
    }

private:
    fs::path path_;
};

bool create_test_file(const fs::path &path)
{
    const HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    constexpr char contents[] = "HardLinkRemover test data";
    DWORD written = 0;
    const BOOL result = WriteFile(
        file,
        contents,
        static_cast<DWORD>(sizeof(contents) - 1U),
        &written,
        nullptr);
    CloseHandle(file);
    return result != FALSE && written == sizeof(contents) - 1U;
}

} // namespace

int main()
{
    wchar_t temporary_root_buffer[MAX_PATH + 1]{};
    const DWORD root_length = GetTempPathW(MAX_PATH, temporary_root_buffer);
    if (root_length == 0 || root_length >= MAX_PATH)
    {
        std::cerr << "Unable to resolve the Windows temporary directory.\n";
        return 1;
    }

    const fs::path test_root =
        fs::path(temporary_root_buffer) /
        (L"HardLinkRemover-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
         std::to_wstring(GetTickCount64()));
    TemporaryDirectory cleanup(test_root);

    std::error_code directory_error;
    fs::create_directories(cleanup.path(), directory_error);
    if (directory_error)
    {
        std::cerr << "Unable to create the test directory.\n";
        return 1;
    }

    const fs::path original = cleanup.path() / L"original-测试.txt";
    const fs::path first_link = cleanup.path() / L"first link.txt";
    const fs::path second_link = cleanup.path() / L"second-link.txt";

    expect(create_test_file(original), "create the original test file");
    expect(CreateHardLinkW(first_link.c_str(), original.c_str(), nullptr) != FALSE, "create the first hard link");
    expect(CreateHardLinkW(second_link.c_str(), original.c_str(), nullptr) != FALSE, "create the second hard link");

    const hardlink_remover::LinkQueryResult initial_query =
        hardlink_remover::find_hard_links(first_link);
    expect(initial_query.succeeded(), "enumerate a hard-link group");
    expect(initial_query.links.size() == 3U, "enumeration returns all three names");
    expect(contains_path(initial_query.links, absolute_path(original)), "enumeration contains the original name");
    expect(contains_path(initial_query.links, absolute_path(first_link)), "enumeration contains the first link");
    expect(contains_path(initial_query.links, absolute_path(second_link)), "enumeration contains the second link");

    const std::vector<hardlink_remover::DeleteResult> selected_results =
        hardlink_remover::delete_hard_links({first_link, second_link});
    expect(selected_results.size() == 2U, "return one result per selected link");
    expect(selected_results.size() >= 2U && selected_results[0].removed && selected_results[1].removed,
           "delete exactly two selected links");
    expect(fs::exists(original), "keep the unselected name");
    expect(!fs::exists(first_link) && !fs::exists(second_link), "selected names no longer exist");

    const hardlink_remover::LinkQueryResult single_query =
        hardlink_remover::find_hard_links(original);
    expect(single_query.succeeded() && single_query.links.size() == 1U,
           "the remaining file has one directory entry");

    const std::vector<hardlink_remover::DeleteResult> protected_result =
        hardlink_remover::delete_hard_links({original});
    expect(protected_result.size() == 1U && !protected_result[0].removed,
           "refuse to delete an ordinary single-link file");
    expect(fs::exists(original), "ordinary file remains after refused deletion");

    expect(CreateHardLinkW(first_link.c_str(), original.c_str(), nullptr) != FALSE,
           "recreate the first hard link");
    expect(CreateHardLinkW(second_link.c_str(), original.c_str(), nullptr) != FALSE,
           "recreate the second hard link");

    const std::vector<hardlink_remover::DeleteResult> all_results =
        hardlink_remover::delete_hard_links({original, first_link, second_link});
    expect(all_results.size() == 3U, "return three results when deleting every name");
    expect(all_results.size() >= 3U && all_results[0].removed && all_results[1].removed && all_results[2].removed,
           "delete every name after validating the whole selection");
    expect(!fs::exists(original) && !fs::exists(first_link) && !fs::exists(second_link),
           "no names remain after deleting the complete group");

    const hardlink_remover::LinkQueryResult missing_query =
        hardlink_remover::find_hard_links(cleanup.path() / L"missing.txt");
    expect(!missing_query.succeeded(), "report an error for a missing file");

    if (failures != 0)
    {
        std::cerr << failures << " test assertion(s) failed.\n";
        return 1;
    }

    std::cout << "All hardlink_core tests passed.\n";
    return 0;
}
