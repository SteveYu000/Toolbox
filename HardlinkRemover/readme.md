# HardLinkRemover

HardLinkRemover is a simple Windows command-line tool designed to find and delete all hard links associated with specified files. It uses the Windows `fsutil` command to retrieve the list of hard links, and the C++ Standard Library (`std::filesystem`) to handle file deletion.

## Description

The main features of this program are as follows:

- Accepts one or more file paths either dragged into the command line or passed directly as arguments.
- Identifies and deletes all hard links associated with these files.

## Compilation Requirements

- MSVC compiler supporting C++17 or later.
- Windows platform.
- Requires linking against `shell32.lib`.
- Make sure to use `/utf-8` encoding.

### Example Compilation Command (MSVC):

```bash
cl /EHsc /std:c++17 /utf-8 /O2 hardlinkremover.cpp
```

# HardLinkRemover

HardLinkRemover 是一个简单的 Windows 命令行工具，用于查找并删除指定文件的所有硬链接（Hard Links）。它通过调用 Windows 的 `fsutil` 命令获取硬链接列表，并使用 C++ 标准库（`std::filesystem`）来处理文件删除操作。

## 说明

本程序的主要功能如下：

- 接收用户拖入或命令行传入的一个或多个文件路径。
- 确认并删除这些文件的所有硬链接。

## 编译要求

- 支持 C++17 或更高版本的MSVC。
- Windows 平台。
- 需要链接 `shell32.lib`。
- 注意使用`/utf-8`。

### 编译命令示例（MSVC）：

```bash
cl /EHsc /std:c++17 /utf-8  /O2  hardlinkremover.cpp
```
