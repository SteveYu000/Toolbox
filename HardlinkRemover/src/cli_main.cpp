#include "hardlink_core.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using hardlink_remover::LinkQueryResult;

namespace
{

constexpr const char *version_text = "2.0.0";

enum class TextLanguage
{
    english,
    chinese
};

TextLanguage text_language = TextLanguage::english;

TextLanguage detect_text_language()
{
    const LANGID language_id = GetUserDefaultUILanguage();
    return PRIMARYLANGID(language_id) == LANG_CHINESE
               ? TextLanguage::chinese
               : TextLanguage::english;
}

bool uses_chinese()
{
    return text_language == TextLanguage::chinese;
}

const char *localized(const char *english, const char *chinese)
{
    return uses_chinese() ? chinese : english;
}

std::string utf8(const std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return localized("<text too long>", "<文本过长>");
    }

    const int source_length = static_cast<int>(text.size());
    const int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        source_length,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0)
    {
        return localized("<encoding error>", "<编码错误>");
    }

    std::string output(static_cast<std::size_t>(required), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        source_length,
        output.data(),
        required,
        nullptr,
        nullptr);
    if (written != required)
    {
        return localized("<encoding error>", "<编码错误>");
    }
    return output;
}

std::string display_path(const fs::path &path)
{
    return utf8(path.wstring());
}

std::wstring localized_core_error(const std::wstring_view message)
{
    if (!uses_chinese())
    {
        return std::wstring(message);
    }

    struct Translation
    {
        std::wstring_view english;
        std::wstring_view chinese;
    };

    constexpr Translation translations[] = {
        {L"Unable to resolve the path", L"无法解析路径"},
        {L"Unable to access the file", L"无法访问文件"},
        {L"The path is a directory, not a file", L"此路径是文件夹而不是文件"},
        {L"Symbolic links and other reparse points are not supported", L"不支持符号链接及其他重解析点"},
        {L"Unable to determine the volume root", L"无法确定卷根目录"},
        {L"Unable to enumerate hard links", L"无法枚举硬链接"},
        {L"Unable to continue enumerating hard links", L"无法继续枚举硬链接"},
        {L"Refusing to delete a directory", L"拒绝删除文件夹"},
        {L"Refusing to delete a reparse point", L"拒绝删除重解析点"},
        {L"Unable to inspect the file", L"无法检查文件"},
        {L"Unable to read hard-link information", L"无法读取硬链接信息"},
        {L"Refusing to delete a file that no longer has multiple hard links", L"文件已不再拥有多个硬链接，拒绝删除"},
        {L"Unable to delete the hard link", L"无法删除硬链接"},
    };

    std::wstring output(message);
    for (const Translation &translation : translations)
    {
        if (output.compare(0, translation.english.size(), translation.english) == 0)
        {
            output.replace(0, translation.english.size(), translation.chinese);
            break;
        }
    }

    constexpr std::wstring_view unknown_error = L"Unknown Windows error";
    constexpr std::wstring_view localized_unknown_error = L"未知的 Windows 错误";
    const std::size_t unknown_position = output.find(unknown_error);
    if (unknown_position != std::wstring::npos)
    {
        output.replace(unknown_position, unknown_error.size(), localized_unknown_error);
    }
    return output;
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

void append_unique(std::vector<fs::path> &paths, const fs::path &candidate)
{
    const bool exists = std::any_of(
        paths.begin(),
        paths.end(),
        [&candidate](const fs::path &path)
        {
            return paths_equal(path, candidate);
        });
    if (!exists)
    {
        paths.push_back(candidate);
    }
}

void print_usage()
{
    std::cout << "HardLinkRemover " << version_text << "\n\n";
    if (uses_chinese())
    {
        std::cout
            << "用法：\n"
            << "  HardLinkRemover list <文件> [文件 ...]\n"
            << "  HardLinkRemover delete [--yes] <硬链接路径> [硬链接路径 ...]\n"
            << "  HardLinkRemover delete-all [--yes] <文件> [文件 ...]\n"
            << "  HardLinkRemover select <文件> [文件 ...]\n"
            << "  HardLinkRemover <文件> [文件 ...]   （等同于 'select'）\n\n"
            << "命令：\n"
            << "  list        显示每个文件的全部名称（硬链接）。\n"
            << "  delete      只删除明确指定的一个或多个硬链接路径。\n"
            << "  delete-all  查找并删除每个指定文件的全部名称。\n"
            << "  select      查找硬链接，再按 1,3-5 等编号选择。\n\n"
            << "选项：\n"
            << "  -y, --yes   跳过最终删除确认。\n"
            << "  -h, --help  显示此帮助。\n\n"
            << "警告：如果选中某个文件的全部名称，其数据将被永久删除。\n";
        return;
    }

    std::cout
        << "Usage:\n"
        << "  HardLinkRemover list <file> [file ...]\n"
        << "  HardLinkRemover delete [--yes] <link> [link ...]\n"
        << "  HardLinkRemover delete-all [--yes] <file> [file ...]\n"
        << "  HardLinkRemover select <file> [file ...]\n"
        << "  HardLinkRemover <file> [file ...]   (same as 'select')\n\n"
        << "Commands:\n"
        << "  list        Show every name (hard link) of each file.\n"
        << "  delete      Delete exactly the supplied one or more link paths.\n"
        << "  delete-all  Find and delete every name of each supplied file.\n"
        << "  select      Find links, then choose indices such as 1,3-5.\n\n"
        << "Options:\n"
        << "  -y, --yes   Skip the final deletion confirmation.\n"
        << "  -h, --help  Show this help.\n\n"
        << "Warning: selecting every name of a file permanently deletes its data.\n";
}

std::string ascii_lower(std::string text)
{
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
        [](const unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return text;
}

bool confirmation()
{
    std::cout << localized("Continue? [y/N] ", "是否继续？[y/N] ") << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer))
    {
        return false;
    }

    answer = ascii_lower(answer);
    return answer == "y" || answer == "yes" ||
           (uses_chinese() && (answer == "是" || answer == "确认"));
}

struct LinkGroup
{
    std::vector<fs::path> links;
};

bool groups_overlap(const LinkGroup &left, const LinkGroup &right)
{
    return std::any_of(
        left.links.begin(),
        left.links.end(),
        [&right](const fs::path &left_path)
        {
            return std::any_of(
                right.links.begin(),
                right.links.end(),
                [&left_path](const fs::path &right_path)
                {
                    return paths_equal(left_path, right_path);
                });
        });
}

void print_query_error(const LinkQueryResult &query)
{
    std::cerr << localized("[error] ", "[错误] ") << display_path(query.requested_path)
              << ": " << utf8(localized_core_error(query.error_message)) << '\n';
}

void print_link_group(
    const LinkGroup &group,
    const std::size_t group_number,
    const std::size_t first_path_number)
{
    if (uses_chinese())
    {
        std::cout << "┌─【硬链接组 #" << group_number << "】（共 "
                  << group.links.size() << " 个路径）\n";
    }
    else
    {
        std::cout << "┌─[Hard-link group #" << group_number << "] ("
                  << group.links.size() << " path(s))\n";
    }
    for (std::size_t index = 0; index < group.links.size(); ++index)
    {
        std::cout << "│  [" << first_path_number + index << "] "
                  << display_path(group.links[index]) << '\n';
    }
    std::cout << "└────────────────────────────────────────\n";
}

void print_link_groups(
    const std::vector<LinkGroup> &groups,
    const bool only_multiple_links,
    const bool continuous_path_numbers)
{
    std::size_t displayed_group_count = 0;
    std::size_t next_path_number = 1;
    for (const LinkGroup &group : groups)
    {
        if (only_multiple_links && group.links.size() < 2U)
        {
            continue;
        }
        if (displayed_group_count != 0U)
        {
            std::cout << '\n';
        }

        ++displayed_group_count;
        print_link_group(
            group,
            displayed_group_count,
            continuous_path_numbers ? next_path_number : 1U);
        if (continuous_path_numbers)
        {
            next_path_number += group.links.size();
        }
    }
}

struct QueryCollection
{
    std::vector<LinkGroup> groups;
    std::vector<fs::path> links;
    std::size_t errors = 0;
    std::size_t single_link_files = 0;
};

QueryCollection collect_links(
    const std::vector<fs::path> &files,
    const bool report_single_link_files)
{
    QueryCollection collection;
    for (const fs::path &file : files)
    {
        LinkQueryResult query = hardlink_remover::find_hard_links(file);

        if (!query.succeeded())
        {
            print_query_error(query);
            ++collection.errors;
            continue;
        }

        LinkGroup candidate{std::move(query.links)};
        const bool duplicate_group = std::any_of(
            collection.groups.begin(),
            collection.groups.end(),
            [&candidate](const LinkGroup &existing)
            {
                return groups_overlap(existing, candidate);
            });
        if (duplicate_group)
        {
            continue;
        }

        const bool has_multiple_links = candidate.links.size() >= 2U;
        collection.groups.push_back(std::move(candidate));
        const LinkGroup &added_group = collection.groups.back();
        if (!has_multiple_links)
        {
            ++collection.single_link_files;
            if (report_single_link_files)
            {
                std::cout << localized("No multiple hard links: ", "没有多个硬链接：")
                          << display_path(query.requested_path) << '\n';
            }
            continue;
        }

        for (const fs::path &link : added_group.links)
        {
            append_unique(collection.links, link);
        }
    }
    return collection;
}

int delete_paths(const std::vector<fs::path> &paths, const bool assume_yes)
{
    if (paths.empty())
    {
        std::cout << localized("Nothing to delete.\n", "没有可删除的路径。\n");
        return 0;
    }

    if (uses_chinese())
    {
        std::cout << "将删除以下 " << paths.size() << " 个路径：\n";
    }
    else
    {
        std::cout << "The following " << paths.size() << " path(s) will be deleted:\n";
    }
    for (const fs::path &path : paths)
    {
        std::cout << "  " << display_path(path) << '\n';
    }
    std::cout << localized(
        "If these are all names of a file, its data will be permanently deleted.\n",
        "如果其中包含某个文件的全部名称，其数据将被永久删除。\n");

    if (!assume_yes && !confirmation())
    {
        std::cout << localized("Cancelled.\n", "已取消。\n");
        return 0;
    }

    const std::vector<hardlink_remover::DeleteResult> results =
        hardlink_remover::delete_hard_links(paths);
    std::size_t removed_count = 0;
    std::size_t failed_count = 0;
    for (const hardlink_remover::DeleteResult &result : results)
    {
        if (result.removed)
        {
            ++removed_count;
            std::cout << localized("Removed: ", "已删除：") << display_path(result.path) << '\n';
        }
        else
        {
            ++failed_count;
            std::cerr << localized("Failed: ", "删除失败：") << display_path(result.path)
                      << ": " << utf8(localized_core_error(result.error_message)) << '\n';
        }
    }

    if (uses_chinese())
    {
        std::cout << "已删除 " << removed_count << " 个路径";
        if (failed_count != 0U)
        {
            std::cout << "；" << failed_count << " 个失败";
        }
        std::cout << "。\n";
    }
    else
    {
        std::cout << "Removed " << removed_count << " path(s)";
        if (failed_count != 0U)
        {
            std::cout << "; " << failed_count << " failed";
        }
        std::cout << ".\n";
    }
    return failed_count == 0U ? 0 : 3;
}

std::string trim_ascii(std::string value)
{
    const auto is_not_space = [](const unsigned char character)
    {
        return std::isspace(character) == 0;
    };
    const auto begin = std::find_if(value.begin(), value.end(), is_not_space);
    const auto end = std::find_if(value.rbegin(), value.rend(), is_not_space).base();
    if (begin >= end)
    {
        return {};
    }
    return std::string(begin, end);
}

bool parse_positive_index(const std::string &text, std::size_t &value)
{
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](const unsigned char character)
                                    { return std::isdigit(character) != 0; }))
    {
        return false;
    }

    try
    {
        const unsigned long long parsed = std::stoull(text);
        if (parsed == 0 || parsed > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()))
        {
            return false;
        }
        value = static_cast<std::size_t>(parsed);
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

std::optional<std::vector<std::size_t>> parse_selection(
    const std::string &input,
    const std::size_t item_count)
{
    const std::string normalized = ascii_lower(trim_ascii(input));
    if (normalized == "q" || normalized == "quit" || normalized == "cancel")
    {
        return std::vector<std::size_t>{};
    }
    if (normalized == "a" || normalized == "all")
    {
        std::vector<std::size_t> all;
        all.reserve(item_count);
        for (std::size_t index = 0; index < item_count; ++index)
        {
            all.push_back(index);
        }
        return all;
    }

    std::set<std::size_t> selected;
    std::size_t segment_start = 0;
    while (segment_start <= normalized.size())
    {
        const std::size_t comma = normalized.find(',', segment_start);
        const std::string segment = trim_ascii(normalized.substr(
            segment_start,
            comma == std::string::npos ? std::string::npos : comma - segment_start));
        if (segment.empty())
        {
            return std::nullopt;
        }

        const std::size_t dash = segment.find('-');
        if (dash == std::string::npos)
        {
            std::size_t index = 0;
            if (!parse_positive_index(segment, index) || index > item_count)
            {
                return std::nullopt;
            }
            selected.insert(index - 1U);
        }
        else
        {
            if (segment.find('-', dash + 1U) != std::string::npos)
            {
                return std::nullopt;
            }
            std::size_t first = 0;
            std::size_t last = 0;
            if (!parse_positive_index(trim_ascii(segment.substr(0, dash)), first) ||
                !parse_positive_index(trim_ascii(segment.substr(dash + 1U)), last) ||
                first > last || last > item_count)
            {
                return std::nullopt;
            }
            for (std::size_t index = first; index <= last; ++index)
            {
                selected.insert(index - 1U);
            }
        }

        if (comma == std::string::npos)
        {
            break;
        }
        segment_start = comma + 1U;
    }

    if (selected.empty())
    {
        return std::nullopt;
    }
    return std::vector<std::size_t>(selected.begin(), selected.end());
}

int select_and_delete(const std::vector<fs::path> &files, const bool assume_yes)
{
    const QueryCollection collection = collect_links(files, true);
    if (collection.links.empty())
    {
        std::cout << localized(
            "No hard-link groups with multiple names were found.\n",
            "未找到包含多个名称的硬链接组。\n");
        return collection.errors == 0U ? 0 : 2;
    }

    std::cout << localized("Available hard links:\n", "可选的硬链接：\n");
    print_link_groups(collection.groups, true, true);

    for (;;)
    {
        std::cout << localized(
                         "Choose links (for example 1,3-5; 'all'; or 'q'): ",
                         "请选择硬链接（例如 1,3-5；输入 'all' 全选；输入 'q' 取消）：")
                  << std::flush;
        std::string input;
        if (!std::getline(std::cin, input))
        {
            std::cout << localized("\nCancelled.\n", "\n已取消。\n");
            return collection.errors == 0U ? 0 : 2;
        }

        const auto selection = parse_selection(input, collection.links.size());
        if (!selection.has_value())
        {
            std::cerr << localized(
                "Invalid selection. Use comma-separated indices and ranges.\n",
                "选择无效。请使用逗号分隔的编号或范围。\n");
            continue;
        }
        if (selection->empty())
        {
            std::cout << localized("Cancelled.\n", "已取消。\n");
            return collection.errors == 0U ? 0 : 2;
        }

        std::vector<fs::path> selected_paths;
        selected_paths.reserve(selection->size());
        for (const std::size_t index : *selection)
        {
            selected_paths.push_back(collection.links[index]);
        }
        return delete_paths(selected_paths, assume_yes);
    }
}

enum class Command
{
    list,
    delete_selected,
    delete_all,
    select
};

bool is_help(const std::wstring_view argument)
{
    return argument == L"-h" || argument == L"--help" || argument == L"/?" || argument == L"help";
}

} // namespace

int wmain(const int argc, wchar_t *argv[])
{
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    text_language = detect_text_language();

    if (argc < 2)
    {
        print_usage();
        return 1;
    }
    if (is_help(argv[1]))
    {
        print_usage();
        return 0;
    }

    Command command = Command::select;
    int path_start = 1;
    const std::wstring_view first = argv[1];
    if (first == L"list" || first == L"--list")
    {
        command = Command::list;
        path_start = 2;
    }
    else if (first == L"delete" || first == L"--delete")
    {
        command = Command::delete_selected;
        path_start = 2;
    }
    else if (first == L"delete-all" || first == L"--delete-all")
    {
        command = Command::delete_all;
        path_start = 2;
    }
    else if (first == L"select" || first == L"--select")
    {
        command = Command::select;
        path_start = 2;
    }
    else if (!first.empty() && first.front() == L'-')
    {
        std::cerr << localized("Unknown command or option: ", "未知命令或选项：")
                  << utf8(first) << "\n\n";
        print_usage();
        return 1;
    }

    bool assume_yes = false;
    bool literal_paths = false;
    std::vector<fs::path> paths;
    for (int index = path_start; index < argc; ++index)
    {
        const std::wstring_view argument = argv[index];
        if (!literal_paths && argument == L"--")
        {
            literal_paths = true;
            continue;
        }
        if (!literal_paths && (argument == L"-y" || argument == L"--yes"))
        {
            assume_yes = true;
            continue;
        }
        if (!literal_paths && is_help(argument))
        {
            print_usage();
            return 0;
        }
        if (!literal_paths && !argument.empty() && argument.front() == L'-')
        {
            std::cerr << localized("Unknown option: ", "未知选项：")
                      << utf8(argument) << '\n';
            return 1;
        }
        paths.emplace_back(argument);
    }

    if (paths.empty())
    {
        std::cerr << localized(
            "At least one file path is required.\n\n",
            "至少需要一个文件路径。\n\n");
        print_usage();
        return 1;
    }
    if (command == Command::list && assume_yes)
    {
        std::cerr << localized(
            "--yes is not valid with the list command.\n",
            "list 命令不能使用 --yes。\n");
        return 1;
    }

    switch (command)
    {
    case Command::list:
    {
        const QueryCollection collection = collect_links(paths, false);
        print_link_groups(collection.groups, false, false);
        return collection.errors == 0U ? 0 : 2;
    }
    case Command::delete_selected:
        return delete_paths(paths, assume_yes);
    case Command::delete_all:
    {
        const QueryCollection collection = collect_links(paths, true);
        const int deletion_result = delete_paths(collection.links, assume_yes);
        if (deletion_result != 0)
        {
            return deletion_result;
        }
        return collection.errors == 0U ? 0 : 2;
    }
    case Command::select:
        return select_and_delete(paths, assume_yes);
    }

    return 1;
}
