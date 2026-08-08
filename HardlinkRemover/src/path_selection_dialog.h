#pragma once

#include <windows.h>

#include <filesystem>
#include <vector>

namespace hardlink_remover
{

// 打开可同时选择普通文件和文件夹的 Explorer 选择窗口。
// 返回 true 表示用户确认，selected_paths 中保存确认时选中的文件系统路径。
[[nodiscard]] bool choose_files_and_folders(
    HINSTANCE instance,
    HWND owner,
    std::vector<std::filesystem::path> &selected_paths);

} // namespace hardlink_remover
