#include "hardlink_core.h"
#include "path_selection_dialog.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

namespace fs = std::filesystem;

namespace
{

constexpr wchar_t window_class_name[] = L"Toolbox.HardLinkRemoverGUI.Window";

constexpr int control_add_paths = 1001;
constexpr int control_remove_selected = 1002;
constexpr int control_refresh = 1003;
constexpr int control_check_all = 1004;
constexpr int control_uncheck_all = 1005;
constexpr int control_clear = 1006;
constexpr int control_delete_checked = 1007;
constexpr int control_link_tree = 1101;
constexpr UINT default_dpi = 96;
constexpr UINT select_tree_item_message = WM_APP + 1U;
constexpr UINT menu_add_paths = 2001;
constexpr UINT menu_toggle_check = 2003;
constexpr UINT menu_check_group = 2004;
constexpr UINT menu_uncheck_group = 2005;
constexpr UINT menu_remove_group = 2006;
constexpr UINT menu_copy_path = 2007;
constexpr UINT menu_delete_checked = 2008;
constexpr UINT menu_refresh = 2009;
constexpr UINT menu_clear = 2010;

struct LinkGroup
{
    fs::path source;
    std::vector<fs::path> links;
};

struct TreeGroupItems
{
    std::size_t group_index = 0;
    HTREEITEM root = nullptr;
    std::vector<HTREEITEM> links;
};

struct FileIdentity
{
    DWORD volume_serial = 0;
    ULONGLONG file_index = 0;

    [[nodiscard]] bool operator<(const FileIdentity &other) const noexcept
    {
        if (volume_serial != other.volume_serial)
        {
            return volume_serial < other.volume_serial;
        }
        return file_index < other.file_index;
    }
};

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

bool is_existing_file(const fs::path &path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool inspect_file_identity(
    const fs::path &path,
    FileIdentity &identity,
    DWORD &link_count)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        return false;
    }

    const HANDLE file = CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    BY_HANDLE_FILE_INFORMATION information{};
    const BOOL succeeded = GetFileInformationByHandle(file, &information);
    CloseHandle(file);
    if (succeeded == FALSE)
    {
        return false;
    }

    identity.volume_serial = information.dwVolumeSerialNumber;
    identity.file_index =
        (static_cast<ULONGLONG>(information.nFileIndexHigh) << 32U) |
        information.nFileIndexLow;
    link_count = information.nNumberOfLinks;
    return true;
}

void append_limited_error(std::wstring &errors, const std::wstring &message)
{
    constexpr std::size_t maximum_length = 6000;
    if (errors.size() >= maximum_length)
    {
        return;
    }
    if (!errors.empty())
    {
        if (errors.size() + 4U >= maximum_length)
        {
            return;
        }
        errors += L"\r\n\r\n";
    }
    const std::size_t remaining = maximum_length - errors.size();
    if (message.size() <= remaining)
    {
        errors += message;
    }
    else
    {
        errors += message.substr(0, remaining - 1U);
        errors += L'…';
    }
}

UINT system_dpi()
{
    const HDC screen = GetDC(nullptr);
    if (screen == nullptr)
    {
        return default_dpi;
    }

    const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    return dpi > 0 ? static_cast<UINT>(dpi) : default_dpi;
}

UINT window_dpi(const HWND window)
{
    using GetDpiForWindowFunction = UINT(WINAPI *)(HWND);
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto get_dpi_for_window = reinterpret_cast<GetDpiForWindowFunction>(
        user32 == nullptr ? nullptr : GetProcAddress(user32, "GetDpiForWindow"));
    if (get_dpi_for_window != nullptr)
    {
        const UINT dpi = get_dpi_for_window(window);
        if (dpi != 0)
        {
            return dpi;
        }
    }
    return system_dpi();
}

class Application
{
public:
    bool create(const HINSTANCE instance, const int show_command)
    {
        instance_ = instance;
        dpi_ = system_dpi();

        INITCOMMONCONTROLSEX common_controls{};
        common_controls.dwSize = sizeof(common_controls);
        common_controls.dwICC = ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES;
        if (InitCommonControlsEx(&common_controls) == FALSE)
        {
            MessageBoxW(nullptr, L"无法初始化 Windows 公共控件。", L"HardLinkRemover", MB_OK | MB_ICONERROR);
            return false;
        }

        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = &Application::window_procedure;
        window_class.hInstance = instance_;
        window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = window_class_name;
        window_class.hIconSm = LoadIconW(nullptr, IDI_APPLICATION);
        if (RegisterClassExW(&window_class) == 0)
        {
            MessageBoxW(nullptr, L"无法注册主窗口。", L"HardLinkRemover", MB_OK | MB_ICONERROR);
            return false;
        }

        window_ = CreateWindowExW(
            WS_EX_ACCEPTFILES,
            window_class_name,
            L"HardLinkRemover v2.0",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            scale(1000),
            scale(640),
            nullptr,
            nullptr,
            instance_,
            this);
        if (window_ == nullptr)
        {
            MessageBoxW(nullptr, L"无法创建主窗口。", L"HardLinkRemover", MB_OK | MB_ICONERROR);
            return false;
        }

        ShowWindow(window_, show_command);
        UpdateWindow(window_);
        return true;
    }

    int run() const
    {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    static LRESULT CALLBACK window_procedure(
        const HWND window,
        const UINT message,
        const WPARAM w_param,
        const LPARAM l_param)
    {
        Application *application = reinterpret_cast<Application *>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto *create = reinterpret_cast<const CREATESTRUCTW *>(l_param);
            application = static_cast<Application *>(create->lpCreateParams);
            application->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(application));
        }

        if (application != nullptr)
        {
            return application->handle_message(message, w_param, l_param);
        }
        return DefWindowProcW(window, message, w_param, l_param);
    }

    LRESULT handle_message(const UINT message, const WPARAM w_param, const LPARAM l_param)
    {
        switch (message)
        {
        case WM_CREATE:
            dpi_ = window_dpi(window_);
            if (!create_controls())
            {
                return -1;
            }
            DragAcceptFiles(window_, TRUE);
            return 0;

        case WM_SIZE:
            layout_controls(LOWORD(l_param), HIWORD(l_param));
            return 0;

        case WM_GETMINMAXINFO:
        {
            auto *information = reinterpret_cast<MINMAXINFO *>(l_param);
            information->ptMinTrackSize.x = scale(820);
            information->ptMinTrackSize.y = scale(460);
            return 0;
        }

        case WM_DPICHANGED:
        {
            dpi_ = LOWORD(w_param);
            if (dpi_ == 0)
            {
                dpi_ = default_dpi;
            }
            recreate_font();

            const auto *suggested = reinterpret_cast<const RECT *>(l_param);
            SetWindowPos(
                window_,
                nullptr,
                suggested->left,
                suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOACTIVATE | SWP_NOZORDER);
            RedrawWindow(
                window_,
                nullptr,
                nullptr,
                RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
            return 0;
        }

        case WM_SETTINGCHANGE:
            recreate_font();
            return 0;

        case WM_COMMAND:
            handle_command(LOWORD(w_param));
            return 0;

        case WM_NOTIFY:
            return handle_notification(reinterpret_cast<const NMHDR *>(l_param));

        case select_tree_item_message:
            TreeView_SelectItem(tree_view_, reinterpret_cast<HTREEITEM>(l_param));
            return 0;

        case WM_CTLCOLORSTATIC:
            if (reinterpret_cast<HWND>(l_param) == instruction_ ||
                reinterpret_cast<HWND>(l_param) == status_)
            {
                const HDC device_context = reinterpret_cast<HDC>(w_param);
                SetTextColor(device_context, GetSysColor(COLOR_WINDOWTEXT));
                SetBkColor(device_context, GetSysColor(COLOR_WINDOW));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            }
            break;

        case WM_DROPFILES:
            add_dropped_files(reinterpret_cast<HDROP>(w_param));
            return 0;

        case WM_DESTROY:
            if (font_ != nullptr && owns_font_)
            {
                DeleteObject(font_);
            }
            font_ = nullptr;
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(window_, message, w_param, l_param);
        }
        return DefWindowProcW(window_, message, w_param, l_param);
    }

    bool create_controls()
    {
        instruction_ = CreateWindowExW(
            0,
            L"STATIC",
            L"添加或拖入文件/文件夹以查找硬链接；展开分组并勾选一个或多个路径后再删除。",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0,
            0,
            0,
            0,
            window_,
            nullptr,
            instance_,
            nullptr);

        add_button_ = create_button(L"添加文件或文件夹", control_add_paths);
        refresh_button_ = create_button(L"刷新", control_refresh);
        check_all_button_ = create_button(L"全选", control_check_all);
        uncheck_all_button_ = create_button(L"取消全选", control_uncheck_all);
        remove_selected_button_ = create_button(L"移出列表", control_remove_selected);
        clear_button_ = create_button(L"清空列表", control_clear);
        delete_button_ = create_button(L"删除已勾选", control_delete_checked);

        tree_view_ = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_TREEVIEWW,
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_CHECKBOXES | TVS_HASBUTTONS |
                TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
            0,
            0,
            0,
            0,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(control_link_tree)),
            instance_,
            nullptr);

        status_ = CreateWindowExW(
            0,
            L"STATIC",
            L"尚未加载文件。",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0,
            0,
            0,
            0,
            window_,
            nullptr,
            instance_,
            nullptr);

        if (instruction_ == nullptr || add_button_ == nullptr || refresh_button_ == nullptr ||
            check_all_button_ == nullptr || uncheck_all_button_ == nullptr || clear_button_ == nullptr ||
            remove_selected_button_ == nullptr || delete_button_ == nullptr ||
            tree_view_ == nullptr || status_ == nullptr)
        {
            MessageBoxW(window_, L"无法创建界面控件。", L"HardLinkRemover", MB_OK | MB_ICONERROR);
            return false;
        }

        recreate_font();

        SendMessageW(
            tree_view_,
            TVM_SETEXTENDEDSTYLE,
            TVS_EX_DOUBLEBUFFER,
            TVS_EX_DOUBLEBUFFER);
        TreeView_SetBkColor(tree_view_, GetSysColor(COLOR_WINDOW));
        TreeView_SetTextColor(tree_view_, GetSysColor(COLOR_WINDOWTEXT));

        update_status();
        return true;
    }

    HWND create_button(const wchar_t *text, const int identifier) const
    {
        return CreateWindowExW(
            0,
            L"BUTTON",
            text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0,
            0,
            0,
            0,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
            instance_,
            nullptr);
    }

    int scale(const int value) const
    {
        return MulDiv(value, static_cast<int>(dpi_), static_cast<int>(default_dpi));
    }

    void recreate_font()
    {
        NONCLIENTMETRICSW metrics{};
        metrics.cbSize = sizeof(metrics);

        HFONT new_font = nullptr;
        bool new_font_is_owned = false;
        bool metrics_are_dpi_adjusted = false;
        using SystemParametersInfoForDpiFunction = BOOL(WINAPI *)(UINT, UINT, PVOID, UINT, UINT);
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        const auto system_parameters_info_for_dpi = reinterpret_cast<SystemParametersInfoForDpiFunction>(
            user32 == nullptr ? nullptr : GetProcAddress(user32, "SystemParametersInfoForDpi"));
        if (system_parameters_info_for_dpi != nullptr)
        {
            metrics_are_dpi_adjusted =
                system_parameters_info_for_dpi(
                    SPI_GETNONCLIENTMETRICS,
                    sizeof(metrics),
                    &metrics,
                    0,
                    dpi_) != FALSE;
        }
        if (!metrics_are_dpi_adjusted)
        {
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
        }

        if (metrics.lfMessageFont.lfFaceName[0] != L'\0')
        {
            const UINT base_dpi = system_dpi();
            if (!metrics_are_dpi_adjusted && base_dpi != 0 && base_dpi != dpi_)
            {
                metrics.lfMessageFont.lfHeight = MulDiv(
                    metrics.lfMessageFont.lfHeight,
                    static_cast<int>(dpi_),
                    static_cast<int>(base_dpi));
            }
            metrics.lfMessageFont.lfQuality = CLEARTYPE_NATURAL_QUALITY;
            new_font = CreateFontIndirectW(&metrics.lfMessageFont);
            new_font_is_owned = new_font != nullptr;
        }
        if (new_font == nullptr)
        {
            new_font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        }

        const HFONT old_font = font_;
        const bool old_font_is_owned = owns_font_;
        font_ = new_font;
        owns_font_ = new_font_is_owned;

        const HWND controls[] = {
            instruction_,
            add_button_,
            refresh_button_,
            check_all_button_,
            uncheck_all_button_,
            remove_selected_button_,
            clear_button_,
            delete_button_,
            tree_view_,
            status_};
        for (const HWND control : controls)
        {
            if (control != nullptr)
            {
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            }
        }

        if (old_font != nullptr && old_font_is_owned)
        {
            DeleteObject(old_font);
        }
    }

    void layout_controls(const int width, const int height) const
    {
        if (tree_view_ == nullptr)
        {
            return;
        }

        const int margin = scale(12);
        const int gap = scale(8);
        const int instruction_height = scale(24);
        const int button_height = scale(32);
        const int status_height = scale(24);
        const int button_top = margin + instruction_height + scale(4);
        const int list_top = button_top + button_height + scale(10);

        MoveWindow(instruction_, margin, margin, std::max(0, width - margin * 2), instruction_height, TRUE);

        int x = margin;
        move_button(add_button_, x, button_top, scale(160), button_height);
        x += scale(160) + gap;
        move_button(refresh_button_, x, button_top, scale(72), button_height);
        x += scale(72) + gap;
        move_button(check_all_button_, x, button_top, scale(72), button_height);
        x += scale(72) + gap;
        move_button(uncheck_all_button_, x, button_top, scale(96), button_height);
        x += scale(96) + gap;
        move_button(remove_selected_button_, x, button_top, scale(112), button_height);
        x += scale(112) + gap;
        move_button(clear_button_, x, button_top, scale(96), button_height);
        x += scale(96) + gap;
        move_button(delete_button_, x, button_top, scale(128), button_height);

        const int list_height = std::max(0, height - list_top - status_height - margin * 2);
        const int content_width = std::max(0, width - margin * 2);
        MoveWindow(tree_view_, margin, list_top, content_width, list_height, TRUE);
        MoveWindow(status_, margin, list_top + list_height + scale(8), content_width, status_height, TRUE);
    }

    static void move_button(
        const HWND button,
        const int x,
        const int y,
        const int width,
        const int height)
    {
        MoveWindow(button, x, y, width, height, TRUE);
    }

    void choose_paths()
    {
        std::vector<fs::path> selected_paths;
        if (hardlink_remover::choose_files_and_folders(instance_, window_, selected_paths))
        {
            add_paths(selected_paths);
        }
    }

    void handle_command(const int identifier)
    {
        switch (identifier)
        {
        case control_add_paths:
            choose_paths();
            break;
        case control_refresh:
            refresh_groups(true);
            break;
        case control_check_all:
            set_all_checks(true);
            break;
        case control_uncheck_all:
            set_all_checks(false);
            break;
        case control_remove_selected:
            remove_selected_items();
            break;
        case control_clear:
            groups_.clear();
            rebuild_tree();
            break;
        case control_delete_checked:
            delete_checked();
            break;
        default:
            break;
        }
    }

    [[nodiscard]] bool tree_item_checked(const HTREEITEM item_handle) const
    {
        TVITEMW item{};
        item.mask = TVIF_HANDLE | TVIF_STATE;
        item.hItem = item_handle;
        item.stateMask = TVIS_STATEIMAGEMASK;
        if (TreeView_GetItem(tree_view_, &item) == FALSE)
        {
            return false;
        }
        return ((item.state & TVIS_STATEIMAGEMASK) >> 12U) == 2U;
    }

    void set_tree_item_checked(const HTREEITEM item_handle, const bool checked) const
    {
        TVITEMW item{};
        item.mask = TVIF_HANDLE | TVIF_STATE;
        item.hItem = item_handle;
        item.stateMask = TVIS_STATEIMAGEMASK;
        item.state = INDEXTOSTATEIMAGEMASK(checked ? 2U : 1U);
        TreeView_SetItem(tree_view_, &item);
    }

    void update_group_root_check(const std::size_t group_index)
    {
        const TreeGroupItems &items = tree_items_[group_index];
        const bool all_checked =
            !items.links.empty() &&
            std::all_of(
                items.links.begin(),
                items.links.end(),
                [this](const HTREEITEM item)
                {
                    return tree_item_checked(item);
                });

        updating_checks_ = true;
        set_tree_item_checked(items.root, all_checked);
        updating_checks_ = false;
    }

    [[nodiscard]] bool locate_tree_item(
        const HTREEITEM item,
        std::size_t &tree_group_index,
        std::optional<std::size_t> &link_index) const
    {
        for (std::size_t index = 0; index < tree_items_.size(); ++index)
        {
            const TreeGroupItems &items = tree_items_[index];
            if (item == items.root)
            {
                tree_group_index = index;
                link_index.reset();
                return true;
            }

            const auto child = std::find(items.links.begin(), items.links.end(), item);
            if (child != items.links.end())
            {
                tree_group_index = index;
                link_index = static_cast<std::size_t>(child - items.links.begin());
                return true;
            }
        }
        return false;
    }

    void set_group_checks(const std::size_t tree_group_index, const bool checked)
    {
        const TreeGroupItems &items = tree_items_[tree_group_index];
        updating_checks_ = true;
        set_tree_item_checked(items.root, checked);
        for (const HTREEITEM child : items.links)
        {
            set_tree_item_checked(child, checked);
        }
        updating_checks_ = false;
        update_status();
    }

    void remove_group_indices(const std::set<std::size_t> &indices)
    {
        if (indices.empty())
        {
            return;
        }

        std::vector<LinkGroup> remaining;
        remaining.reserve(groups_.size() - std::min(groups_.size(), indices.size()));
        for (std::size_t index = 0; index < groups_.size(); ++index)
        {
            if (indices.find(index) == indices.end())
            {
                remaining.push_back(std::move(groups_[index]));
            }
        }

        const std::size_t removed_count = groups_.size() - remaining.size();
        groups_ = std::move(remaining);
        rebuild_tree();
        const std::wstring message =
            L"已从列表移除 " + std::to_wstring(removed_count) +
            L" 组文件；磁盘文件未被删除。";
        SetWindowTextW(status_, message.c_str());
    }

    void remove_link_indices(const std::vector<std::set<std::size_t>> &indices)
    {
        std::size_t removed_count = 0;
        std::vector<LinkGroup> remaining_groups;
        remaining_groups.reserve(groups_.size());
        for (std::size_t group_index = 0; group_index < groups_.size(); ++group_index)
        {
            LinkGroup &group = groups_[group_index];
            std::vector<fs::path> remaining_links;
            remaining_links.reserve(group.links.size());
            for (std::size_t link_index = 0; link_index < group.links.size(); ++link_index)
            {
                if (indices[group_index].find(link_index) != indices[group_index].end())
                {
                    ++removed_count;
                }
                else
                {
                    remaining_links.push_back(std::move(group.links[link_index]));
                }
            }

            if (remaining_links.empty())
            {
                continue;
            }

            group.links = std::move(remaining_links);
            const bool source_is_visible = std::any_of(
                group.links.begin(),
                group.links.end(),
                [&group](const fs::path &path)
                {
                    return paths_equal(path, group.source);
                });
            if (!source_is_visible)
            {
                group.source = group.links.front();
            }
            remaining_groups.push_back(std::move(group));
        }

        groups_ = std::move(remaining_groups);
        rebuild_tree();
        const std::wstring message =
            L"已从列表移除 " + std::to_wstring(removed_count) +
            L" 个路径；磁盘文件未被删除。";
        SetWindowTextW(status_, message.c_str());
    }

    void remove_selected_items()
    {
        std::vector<std::set<std::size_t>> indices(groups_.size());
        bool has_checked_items = false;
        for (const TreeGroupItems &items : tree_items_)
        {
            for (std::size_t link_index = 0; link_index < items.links.size(); ++link_index)
            {
                if (tree_item_checked(items.links[link_index]))
                {
                    indices[items.group_index].insert(link_index);
                    has_checked_items = true;
                }
            }
        }

        if (!has_checked_items)
        {
            const HTREEITEM selected_item = TreeView_GetSelection(tree_view_);
            std::size_t tree_group_index = 0;
            std::optional<std::size_t> link_index;
            if (selected_item == nullptr ||
                !locate_tree_item(selected_item, tree_group_index, link_index))
            {
                MessageBoxW(
                    window_,
                    L"请先勾选路径或选择一个树节点。",
                    L"HardLinkRemover",
                    MB_OK | MB_ICONINFORMATION);
                return;
            }

            const std::size_t group_index = tree_items_[tree_group_index].group_index;
            if (!link_index.has_value())
            {
                remove_group_indices({group_index});
                return;
            }
            indices[group_index].insert(*link_index);
        }

        remove_link_indices(indices);
    }

    [[nodiscard]] bool copy_text_to_clipboard(const std::wstring &text) const
    {
        if (OpenClipboard(window_) == FALSE)
        {
            return false;
        }

        const SIZE_T byte_count = (text.size() + 1U) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, byte_count);
        if (memory == nullptr)
        {
            CloseClipboard();
            return false;
        }

        void *destination = GlobalLock(memory);
        if (destination == nullptr)
        {
            GlobalFree(memory);
            CloseClipboard();
            return false;
        }
        CopyMemory(destination, text.c_str(), byte_count);
        GlobalUnlock(memory);

        EmptyClipboard();
        const bool succeeded = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        CloseClipboard();
        if (!succeeded)
        {
            GlobalFree(memory);
        }
        return succeeded;
    }

    void show_tree_context_menu()
    {
        POINT screen_position{};
        if (GetCursorPos(&screen_position) == FALSE)
        {
            return;
        }

        POINT client_position = screen_position;
        ScreenToClient(tree_view_, &client_position);
        TVHITTESTINFO hit_test{};
        hit_test.pt = client_position;
        const HTREEITEM clicked_item = TreeView_HitTest(tree_view_, &hit_test);

        std::size_t tree_group_index = 0;
        std::optional<std::size_t> link_index;
        const bool has_item =
            clicked_item != nullptr &&
            locate_tree_item(clicked_item, tree_group_index, link_index);
        if (has_item)
        {
            TreeView_SelectItem(tree_view_, clicked_item);
        }

        const HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        AppendMenuW(menu, MF_STRING, menu_add_paths, L"添加文件或文件夹…");

        if (has_item)
        {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(
                menu,
                MF_STRING,
                menu_toggle_check,
                tree_item_checked(clicked_item) ? L"取消勾选" : L"勾选");

            const TreeGroupItems &items = tree_items_[tree_group_index];
            if (link_index.has_value())
            {
                AppendMenuW(
                    menu,
                    MF_STRING,
                    tree_item_checked(items.root) ? menu_uncheck_group : menu_check_group,
                    tree_item_checked(items.root) ? L"取消勾选整组" : L"勾选整组");
            }

            AppendMenuW(
                menu,
                MF_STRING,
                menu_remove_group,
                link_index.has_value() ? L"从列表中移除此路径" : L"从列表中移除此组");
            AppendMenuW(menu, MF_STRING, menu_copy_path, L"复制路径");
        }

        if (!groups_.empty())
        {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            if (!checked_paths().empty())
            {
                AppendMenuW(menu, MF_STRING, menu_delete_checked, L"删除已勾选");
            }
            AppendMenuW(menu, MF_STRING, menu_refresh, L"刷新");
            AppendMenuW(menu, MF_STRING, menu_clear, L"清空列表");
        }

        SetForegroundWindow(window_);
        const UINT command = TrackPopupMenuEx(
            menu,
            TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
            screen_position.x,
            screen_position.y,
            window_,
            nullptr);
        DestroyMenu(menu);
        PostMessageW(window_, WM_NULL, 0, 0);

        if (command == menu_add_paths)
        {
            choose_paths();
        }
        else if (has_item && command == menu_toggle_check)
        {
            set_tree_item_checked(clicked_item, !tree_item_checked(clicked_item));
        }
        else if (has_item && command == menu_check_group)
        {
            set_group_checks(tree_group_index, true);
        }
        else if (has_item && command == menu_uncheck_group)
        {
            set_group_checks(tree_group_index, false);
        }
        else if (has_item && command == menu_remove_group)
        {
            const std::size_t group_index = tree_items_[tree_group_index].group_index;
            if (link_index.has_value())
            {
                std::vector<std::set<std::size_t>> indices(groups_.size());
                indices[group_index].insert(*link_index);
                remove_link_indices(indices);
            }
            else
            {
                remove_group_indices({group_index});
            }
        }
        else if (has_item && command == menu_copy_path)
        {
            const TreeGroupItems &items = tree_items_[tree_group_index];
            const fs::path &path = link_index.has_value()
                                       ? groups_[items.group_index].links[*link_index]
                                       : groups_[items.group_index].source;
            if (!copy_text_to_clipboard(path.wstring()))
            {
                MessageBoxW(window_, L"复制路径失败。", L"HardLinkRemover", MB_OK | MB_ICONERROR);
            }
        }
        else if (command == menu_delete_checked)
        {
            delete_checked();
        }
        else if (command == menu_refresh)
        {
            refresh_groups(true);
        }
        else if (command == menu_clear)
        {
            groups_.clear();
            rebuild_tree();
        }
    }

    LRESULT handle_notification(const NMHDR *notification)
    {
        if (notification == nullptr || notification->hwndFrom != tree_view_)
        {
            return 0;
        }

        if (notification->code == NM_RCLICK)
        {
            show_tree_context_menu();
            return 0;
        }
        if (notification->code == NM_CLICK)
        {
            POINT cursor_position{};
            if (GetCursorPos(&cursor_position) != FALSE &&
                ScreenToClient(tree_view_, &cursor_position) != FALSE)
            {
                TVHITTESTINFO hit_test{};
                hit_test.pt = cursor_position;
                TreeView_HitTest(tree_view_, &hit_test);
                if (hit_test.hItem != nullptr &&
                    (hit_test.flags & TVHT_ONITEMSTATEICON) != 0)
                {
                    PostMessageW(
                        window_,
                        select_tree_item_message,
                        0,
                        reinterpret_cast<LPARAM>(hit_test.hItem));
                }
            }
        }
        else if (notification->code == TVN_ITEMCHANGEDW && !updating_checks_)
        {
            const auto *change = reinterpret_cast<const NMTVITEMCHANGE *>(notification);
            const UINT old_check_state = (change->uStateOld & TVIS_STATEIMAGEMASK) >> 12U;
            const UINT new_check_state = (change->uStateNew & TVIS_STATEIMAGEMASK) >> 12U;
            if ((change->uChanged & TVIF_STATE) == 0 || old_check_state == new_check_state)
            {
                return 0;
            }

            for (std::size_t group_index = 0; group_index < tree_items_.size(); ++group_index)
            {
                TreeGroupItems &items = tree_items_[group_index];
                if (change->hItem == items.root)
                {
                    const bool checked = new_check_state == 2U;
                    updating_checks_ = true;
                    for (const HTREEITEM child : items.links)
                    {
                        set_tree_item_checked(child, checked);
                    }
                    updating_checks_ = false;
                    update_status();
                    return 0;
                }

                const auto child = std::find(items.links.begin(), items.links.end(), change->hItem);
                if (child != items.links.end())
                {
                    update_group_root_check(group_index);
                    update_status();
                    return 0;
                }
            }
        }
        else if (notification->code == TVN_KEYDOWN)
        {
            const auto *key = reinterpret_cast<const NMTVKEYDOWN *>(notification);
            if (key->wVKey == VK_DELETE)
            {
                delete_checked();
            }
        }
        else if (notification->code == TVN_SELCHANGEDW)
        {
            update_status();
        }
        return 0;
    }

    void add_dropped_files(const HDROP drop)
    {
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFFU, nullptr, 0);
        std::vector<fs::path> dropped;
        dropped.reserve(count);
        for (UINT index = 0; index < count; ++index)
        {
            const UINT length = DragQueryFileW(drop, index, nullptr, 0);
            std::vector<wchar_t> path_buffer(static_cast<std::size_t>(length) + 1U, L'\0');
            if (DragQueryFileW(drop, index, path_buffer.data(), length + 1U) != 0)
            {
                dropped.emplace_back(path_buffer.data());
            }
        }
        DragFinish(drop);
        add_paths(dropped);
    }

    void add_paths(const std::vector<fs::path> &inputs)
    {
        struct Candidate
        {
            fs::path path;
            bool report_single_link = false;
        };

        std::vector<Candidate> candidates;
        std::set<FileIdentity> scanned_identities;
        std::wstring errors;
        std::size_t added_count = 0;
        std::size_t duplicate_count = 0;
        std::size_t folder_count = 0;
        std::size_t scanned_file_count = 0;
        std::size_t skipped_count = 0;

        SetWindowTextW(status_, L"正在扫描文件和文件夹，请稍候……");
        UpdateWindow(status_);
        const HCURSOR previous_cursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));

        for (const fs::path &input : inputs)
        {
            const DWORD attributes = GetFileAttributesW(input.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                append_limited_error(errors, input.wstring() + L"\r\n无法访问该路径。");
                continue;
            }
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                candidates.push_back({input, true});
                continue;
            }
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            {
                append_limited_error(errors, input.wstring() + L"\r\n不扫描目录重解析点。");
                continue;
            }

            ++folder_count;
            std::error_code traversal_error;
            fs::recursive_directory_iterator iterator(
                input,
                fs::directory_options::skip_permission_denied,
                traversal_error);
            const fs::recursive_directory_iterator end;
            if (traversal_error)
            {
                append_limited_error(errors, input.wstring() + L"\r\n无法遍历该文件夹。");
                continue;
            }

            while (iterator != end)
            {
                std::error_code type_error;
                const bool is_regular_file = iterator->is_regular_file(type_error);
                if (type_error)
                {
                    ++skipped_count;
                }
                else if (is_regular_file)
                {
                    ++scanned_file_count;
                    FileIdentity identity;
                    DWORD link_count = 0;
                    if (!inspect_file_identity(iterator->path(), identity, link_count))
                    {
                        ++skipped_count;
                    }
                    else if (link_count >= 2U && scanned_identities.insert(identity).second)
                    {
                        candidates.push_back({iterator->path(), false});
                    }
                }

                iterator.increment(traversal_error);
                if (traversal_error)
                {
                    ++skipped_count;
                    traversal_error.clear();
                }
            }
        }

        for (const Candidate &candidate_file : candidates)
        {
            const hardlink_remover::LinkQueryResult query =
                hardlink_remover::find_hard_links(candidate_file.path);
            if (!query.succeeded())
            {
                append_limited_error(
                    errors,
                    query.requested_path.wstring() + L"\r\n" + query.error_message);
                continue;
            }
            if (query.links.size() < 2U)
            {
                if (candidate_file.report_single_link)
                {
                    append_limited_error(
                        errors,
                        query.requested_path.wstring() + L"\r\n该文件当前只有一个名称，没有可选择的多个硬链接。");
                }
                continue;
            }

            LinkGroup candidate{query.requested_path, query.links};
            const auto duplicate = std::find_if(
                groups_.begin(),
                groups_.end(),
                [&candidate](const LinkGroup &group)
                {
                    return groups_overlap(group, candidate);
                });
            if (duplicate != groups_.end())
            {
                *duplicate = std::move(candidate);
                ++duplicate_count;
            }
            else
            {
                groups_.push_back(std::move(candidate));
                ++added_count;
            }
        }

        SetCursor(previous_cursor);
        rebuild_tree();

        if (skipped_count != 0U)
        {
            append_limited_error(
                errors,
                L"扫描时有 " + std::to_wstring(skipped_count) + L" 个文件或目录项因权限或访问错误被跳过。");
        }

        if (!errors.empty())
        {
            MessageBoxW(window_, errors.c_str(), L"部分路径未加载", MB_OK | MB_ICONWARNING);
        }
        else if (folder_count != 0U && added_count == 0U && duplicate_count == 0U)
        {
            const std::wstring message =
                L"已扫描 " + std::to_wstring(scanned_file_count) +
                L" 个文件，没有找到包含多个名称的硬链接组。";
            MessageBoxW(window_, message.c_str(), L"扫描完成", MB_OK | MB_ICONINFORMATION);
        }
        else if (added_count == 0U && duplicate_count != 0U)
        {
            SetWindowTextW(status_, L"这些硬链接组已在列表中，内容已刷新。");
        }
    }

    void refresh_groups(const bool report_errors)
    {
        std::vector<LinkGroup> refreshed;
        std::wstring errors;

        for (const LinkGroup &group : groups_)
        {
            const auto representative = std::find_if(
                group.links.begin(),
                group.links.end(),
                [](const fs::path &path)
                {
                    return is_existing_file(path);
                });
            if (representative == group.links.end())
            {
                continue;
            }

            const hardlink_remover::LinkQueryResult query =
                hardlink_remover::find_hard_links(*representative);
            if (!query.succeeded())
            {
                append_limited_error(errors, representative->wstring() + L"\r\n" + query.error_message);
                continue;
            }
            if (query.links.size() < 2U)
            {
                continue;
            }

            LinkGroup candidate{query.requested_path, query.links};
            const bool duplicate = std::any_of(
                refreshed.begin(),
                refreshed.end(),
                [&candidate](const LinkGroup &existing)
                {
                    return groups_overlap(existing, candidate);
                });
            if (!duplicate)
            {
                refreshed.push_back(std::move(candidate));
            }
        }

        groups_ = std::move(refreshed);
        rebuild_tree();
        if (report_errors && !errors.empty())
        {
            MessageBoxW(window_, errors.c_str(), L"刷新时发生错误", MB_OK | MB_ICONWARNING);
        }
    }

    void rebuild_tree()
    {
        updating_checks_ = true;
        tree_items_.clear();
        tree_items_.reserve(groups_.size());

        SendMessageW(tree_view_, WM_SETREDRAW, FALSE, 0);
        TreeView_DeleteAllItems(tree_view_);

        for (std::size_t group_index = 0; group_index < groups_.size(); ++group_index)
        {
            const LinkGroup &group = groups_[group_index];
            std::wstring group_name = group.source.filename().wstring();
            if (group_name.empty())
            {
                group_name = group.source.wstring();
            }
            const std::wstring group_text =
                L"硬链接组 " + std::to_wstring(group_index + 1U) +
                L"（" + std::to_wstring(group.links.size()) + L" 个路径） · " +
                group_name;

            TVINSERTSTRUCTW root_insert{};
            root_insert.hParent = TVI_ROOT;
            root_insert.hInsertAfter = TVI_LAST;
            root_insert.item.mask = TVIF_TEXT | TVIF_STATE;
            root_insert.item.pszText = const_cast<wchar_t *>(group_text.c_str());
            root_insert.item.stateMask = TVIS_BOLD;
            root_insert.item.state = TVIS_BOLD;

            TreeGroupItems group_items;
            group_items.group_index = group_index;
            group_items.root = TreeView_InsertItem(tree_view_, &root_insert);
            if (group_items.root == nullptr)
            {
                continue;
            }

            group_items.links.reserve(group.links.size());
            for (const fs::path &path : group.links)
            {
                const std::wstring path_text = path.wstring();
                TVINSERTSTRUCTW link_insert{};
                link_insert.hParent = group_items.root;
                link_insert.hInsertAfter = TVI_LAST;
                link_insert.item.mask = TVIF_TEXT;
                link_insert.item.pszText = const_cast<wchar_t *>(path_text.c_str());
                const HTREEITEM link_item = TreeView_InsertItem(tree_view_, &link_insert);
                if (link_item != nullptr)
                {
                    group_items.links.push_back(link_item);
                }
            }

            TreeView_Expand(tree_view_, group_items.root, TVE_EXPAND);
            tree_items_.push_back(std::move(group_items));
        }

        SendMessageW(tree_view_, WM_SETREDRAW, TRUE, 0);
        updating_checks_ = false;
        InvalidateRect(tree_view_, nullptr, TRUE);
        update_status();
    }

    [[nodiscard]] std::vector<fs::path> checked_paths() const
    {
        std::vector<fs::path> checked;
        for (std::size_t group_index = 0; group_index < tree_items_.size(); ++group_index)
        {
            const TreeGroupItems &items = tree_items_[group_index];
            const LinkGroup &group = groups_[items.group_index];
            for (std::size_t link_index = 0; link_index < items.links.size(); ++link_index)
            {
                if (tree_item_checked(items.links[link_index]))
                {
                    checked.push_back(group.links[link_index]);
                }
            }
        }
        return checked;
    }

    [[nodiscard]] std::size_t total_link_count() const
    {
        std::size_t count = 0;
        for (const LinkGroup &group : groups_)
        {
            count += group.links.size();
        }
        return count;
    }

    void set_all_checks(const bool checked)
    {
        updating_checks_ = true;
        for (const TreeGroupItems &items : tree_items_)
        {
            set_tree_item_checked(items.root, checked);
            for (const HTREEITEM child : items.links)
            {
                set_tree_item_checked(child, checked);
            }
        }
        updating_checks_ = false;
        update_status();
    }

    void update_status() const
    {
        if (status_ == nullptr || delete_button_ == nullptr ||
            remove_selected_button_ == nullptr)
        {
            return;
        }
        const std::size_t checked_count = tree_view_ == nullptr ? 0U : checked_paths().size();
        const std::size_t link_count = total_link_count();
        const std::wstring status_text =
            L"共 " + std::to_wstring(groups_.size()) + L" 组文件，" +
            std::to_wstring(link_count) + L" 个硬链接；已勾选 " +
            std::to_wstring(checked_count) + L" 个。";
        SetWindowTextW(status_, status_text.c_str());
        EnableWindow(delete_button_, checked_count == 0U ? FALSE : TRUE);
        EnableWindow(refresh_button_, groups_.empty() ? FALSE : TRUE);
        EnableWindow(check_all_button_, link_count == 0U ? FALSE : TRUE);
        EnableWindow(uncheck_all_button_, checked_count == 0U ? FALSE : TRUE);
        EnableWindow(
            remove_selected_button_,
            (checked_count != 0U || TreeView_GetSelection(tree_view_) != nullptr) ? TRUE : FALSE);
        EnableWindow(clear_button_, groups_.empty() ? FALSE : TRUE);
    }

    void delete_checked()
    {
        const std::vector<fs::path> selected_paths = checked_paths();
        if (selected_paths.empty())
        {
            MessageBoxW(window_, L"请先勾选至少一个硬链接。", L"HardLinkRemover", MB_OK | MB_ICONINFORMATION);
            return;
        }

        std::wstring prompt = L"将删除以下 " + std::to_wstring(selected_paths.size()) + L" 个路径：\r\n\r\n";
        constexpr std::size_t preview_count = 8;
        for (std::size_t index = 0; index < selected_paths.size(); ++index)
        {
            if (index < preview_count)
            {
                prompt += L"• " + selected_paths[index].wstring() + L"\r\n";
            }
        }
        if (selected_paths.size() > preview_count)
        {
            prompt += L"• ……另有 " + std::to_wstring(selected_paths.size() - preview_count) + L" 个\r\n";
        }
        prompt += L"\r\n如果勾选了某个文件的全部名称，其数据将永久删除。确定继续吗？";

        const int answer = MessageBoxW(
            window_,
            prompt.c_str(),
            L"确认删除硬链接",
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (answer != IDYES)
        {
            return;
        }

        const std::vector<hardlink_remover::DeleteResult> results =
            hardlink_remover::delete_hard_links(selected_paths);
        std::size_t removed_count = 0;
        std::wstring failures;
        for (const hardlink_remover::DeleteResult &result : results)
        {
            if (result.removed)
            {
                ++removed_count;
            }
            else
            {
                append_limited_error(
                    failures,
                    result.path.wstring() + L"\r\n" + result.error_message);
            }
        }

        refresh_groups(false);

        if (!failures.empty())
        {
            const std::wstring message =
                L"已删除 " + std::to_wstring(removed_count) + L" 个路径。以下路径删除失败：\r\n\r\n" + failures;
            MessageBoxW(window_, message.c_str(), L"删除未完全成功", MB_OK | MB_ICONWARNING);
        }
        else
        {
            const std::wstring message = L"已删除 " + std::to_wstring(removed_count) + L" 个硬链接路径。";
            MessageBoxW(window_, message.c_str(), L"删除完成", MB_OK | MB_ICONINFORMATION);
        }
    }

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND instruction_ = nullptr;
    HWND add_button_ = nullptr;
    HWND refresh_button_ = nullptr;
    HWND check_all_button_ = nullptr;
    HWND uncheck_all_button_ = nullptr;
    HWND remove_selected_button_ = nullptr;
    HWND clear_button_ = nullptr;
    HWND delete_button_ = nullptr;
    HWND tree_view_ = nullptr;
    HWND status_ = nullptr;
    HFONT font_ = nullptr;
    bool owns_font_ = true;
    UINT dpi_ = default_dpi;
    std::vector<LinkGroup> groups_;
    std::vector<TreeGroupItems> tree_items_;
    bool updating_checks_ = false;
};

} // namespace

int WINAPI wWinMain(
    const HINSTANCE instance,
    HINSTANCE,
    wchar_t *,
    const int show_command)
{
    const HRESULT com_result = CoInitializeEx(
        nullptr,
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool uninitialize_com = SUCCEEDED(com_result);

    int exit_code = 1;
    {
        Application application;
        if (application.create(instance, show_command))
        {
            exit_code = application.run();
        }
    }

    if (uninitialize_com)
    {
        CoUninitialize();
    }
    return exit_code;
}
