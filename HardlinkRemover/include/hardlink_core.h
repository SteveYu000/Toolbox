#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hardlink_remover
{

struct LinkQueryResult
{
    std::filesystem::path requested_path;
    std::vector<std::filesystem::path> links;
    std::uint32_t error_code = 0;
    std::wstring error_message;

    [[nodiscard]] bool succeeded() const noexcept
    {
        return error_code == 0 && error_message.empty();
    }
};

struct DeleteResult
{
    std::filesystem::path path;
    bool removed = false;
    std::uint32_t error_code = 0;
    std::wstring error_message;
};

// 返回所有与 file_path 指向同一文件的目录项。
// 返回值均为绝对路径，并按路径名称进行不区分大小写排序。
[[nodiscard]] LinkQueryResult find_hard_links(const std::filesystem::path &file_path);

// 仅删除明确传入的目录项。任何删除发生前会先验证全部候选项，
// 因此调用者可以有意一次删除文件的全部名称，同时误传普通单链接文件时会拒绝删除。
[[nodiscard]] std::vector<DeleteResult> delete_hard_links(
    const std::vector<std::filesystem::path> &paths);

} // namespace hardlink_remover
