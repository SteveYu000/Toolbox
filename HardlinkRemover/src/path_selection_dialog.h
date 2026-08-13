#pragma once

#include <windows.h>

#include <filesystem>
#include <vector>

namespace hardlink_remover
{

// 打开 Windows 原生文件选择器，并允许一次选择普通文件、文件夹或两者的组合。
// 返回 true 表示用户确认，selected_paths 中保存确认时选中的文件系统路径。
[[nodiscard]] bool choose_files_and_folders(
    HINSTANCE instance,
    HWND owner,
    std::vector<std::filesystem::path> &selected_paths);

} // namespace hardlink_remover
