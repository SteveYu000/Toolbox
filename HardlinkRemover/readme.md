# HardLinkRemover（中文）

HardLinkRemover 是一个用于枚举和删除 Windows 硬链接的工具。2.0 版本同时提供命令行程序和原生 Win32 GUI，并直接调用 Windows 硬链接 API，不再启动或解析 `fsutil`。

## 功能

- 列出所有指向同一文件的目录项。
- CLI 使用带编号的边界标识分别显示每个硬链接组，交互选择时路径编号保持全局连续。
- 精确删除命令中明确指定的一个或多个硬链接路径。
- 命令行交互选择支持 `1,3-5` 这样的编号和范围。
- 保留明确的 `delete-all` 操作，用于删除整个硬链接组。
- 原生 GUI 以树状图分组显示硬链接，支持文件/文件夹拖放、递归扫描文件夹、复选框多选、刷新及批量删除。
- 支持 Unicode 路径；精确删除时拒绝目录、重解析点和普通单链接文件。

## 构建

环境要求：

- Windows
- CMake 3.20 或更高版本
- 支持 C++17 的编译器，例如安装了“使用 C++ 的桌面开发”组件的 Visual Studio 2022

进入 HardLinkRemover 项目目录后独立构建：

```powershell
cd HardlinkRemover
cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build --config Release --parallel
```

使用 Visual Studio 生成器时，可执行文件位于：

```text
build/Release/HardLinkRemover.exe
build/Release/HardLinkRemoverGUI.exe
```

常用 CMake 选项：

```powershell
# 不构建 GUI
cmake -S . -B build -DHARDLINKREMOVER_BUILD_GUI=OFF

# 在独立目录构建并运行测试
cmake -S . -B build-tests -DBUILD_TESTING=ON
cmake --build build-tests --config Debug --parallel
ctest --test-dir build-tests -C Debug --output-on-failure
```

默认情况下不会生成测试程序。显式启用测试时，测试程序会放在独立的 `build-tests/tests/<配置>` 目录，不会进入正式 Release 输出目录。

## 命令行用法

```text
HardLinkRemover list <文件> [文件 ...]
HardLinkRemover delete [--yes] <硬链接路径> [硬链接路径 ...]
HardLinkRemover delete-all [--yes] <文件> [文件 ...]
HardLinkRemover select <文件> [文件 ...]
HardLinkRemover <文件> [文件 ...]
```

示例：

```powershell
# 查看一个硬链接组
HardLinkRemover list "C:\data\source.bin"

# 只删除明确指定的两个路径
HardLinkRemover delete "C:\data\copy-a.bin" "C:\archive\copy-b.bin"

# 枚举硬链接后，按编号交互选择
HardLinkRemover select "C:\data\source.bin"

# 明确删除该文件的全部名称
HardLinkRemover delete-all "C:\data\source.bin"
```

不写子命令而直接传入文件路径时，行为等同于 `select`，因此仍可把文件直接拖到命令行程序上使用。`delete` 和 `delete-all` 可添加 `--yes` 或 `-y` 跳过最终确认。受 Windows 机制限制，同一硬链接组的所有名称必须位于同一个卷中。

CLI 会根据 Windows 当前用户的界面语言自动选择中文或英文。中文环境显示中文帮助、交互提示及执行结果，其他语言环境显示英文；命令名和选项保持不变，输出编码为 UTF-8。

## GUI 用法

运行 `HardLinkRemoverGUI.exe`，点击“添加文件或文件夹”打开统一选择窗口，可以在同一窗口选择文件、文件夹或两者的组合，也可以直接把文件和文件夹拖入主窗口。选择窗口遵循 Explorer 的原生选择方式，默认按住 Ctrl 或 Shift 多选；只有当前用户已在 Explorer 中启用项目复选框时才显示复选框。选择文件夹后会递归扫描。每组硬链接显示为一个可展开的根节点，各硬链接路径显示为子节点；可以只勾选部分子路径，也可以勾选根节点选择整组，然后点击“删除已勾选”。“移出列表”只隐藏勾选的路径或当前节点，不会删除磁盘文件；右键树节点可执行勾选整组、移出列表、复制路径、删除及刷新等操作。

## 安全机制

核心库会在删除前验证所有传入路径，并拒绝目录、符号链接及其他重解析点，也拒绝当前只有一个目录项的普通文件。所有候选路径会先完成验证再开始删除，所以仍然允许用户有意选中并删除某个硬链接文件的全部名称。

删除一个文件的全部名称会永久删除其数据。除非使用 `--yes` 明确关闭确认，否则 CLI 和 GUI 都会在删除前给出风险提示。
