#include "path_selection_dialog.h"

#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <string>
#include <utility>

namespace hardlink_remover
{
namespace
{

constexpr wchar_t selection_window_class_name[] =
    L"Toolbox.HardLinkRemoverGUI.PathSelectionWindow";
constexpr int control_back = 3001;
constexpr int control_forward = 3002;
constexpr int control_parent = 3003;
constexpr UINT default_dpi = 96;

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

bool paths_equal(const std::filesystem::path &left, const std::filesystem::path &right)
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

class SelectionWindow
{
public:
    ~SelectionWindow()
    {
        release_resources();
    }

    bool show(
        const HINSTANCE instance,
        const HWND owner,
        std::vector<std::filesystem::path> &selected_paths)
    {
        instance_ = instance;
        owner_ = owner;
        dpi_ = window_dpi(owner_);
        accepted_ = false;
        selected_paths_.clear();

        if (!register_window_class())
        {
            MessageBoxW(owner_, L"无法注册文件选择窗口。", L"HardLinkRemover", MB_OK | MB_ICONERROR);
            return false;
        }

        const int desired_width = scale(960);
        const int desired_height = scale(640);
        RECT owner_rectangle{};
        GetWindowRect(owner_, &owner_rectangle);

        MONITORINFO monitor_information{};
        monitor_information.cbSize = sizeof(monitor_information);
        const HMONITOR monitor = MonitorFromWindow(owner_, MONITOR_DEFAULTTONEAREST);
        GetMonitorInfoW(monitor, &monitor_information);
        const RECT work_area = monitor_information.rcWork;

        const int work_left = static_cast<int>(work_area.left);
        const int work_top = static_cast<int>(work_area.top);
        const int work_width = static_cast<int>(work_area.right - work_area.left);
        const int work_height = static_cast<int>(work_area.bottom - work_area.top);
        const int width = std::min(desired_width, work_width);
        const int height = std::min(desired_height, work_height);
        const int owner_center_x = static_cast<int>(
            owner_rectangle.left + (owner_rectangle.right - owner_rectangle.left) / 2);
        const int owner_center_y = static_cast<int>(
            owner_rectangle.top + (owner_rectangle.bottom - owner_rectangle.top) / 2);
        const int left = std::clamp(owner_center_x - width / 2, work_left, work_left + work_width - width);
        const int top = std::clamp(owner_center_y - height / 2, work_top, work_top + work_height - height);

        window_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
            selection_window_class_name,
            L"选择文件或文件夹",
            WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX |
                WS_CLIPCHILDREN,
            left,
            top,
            width,
            height,
            owner_,
            nullptr,
            instance_,
            this);
        if (window_ == nullptr)
        {
            MessageBoxW(owner_, L"无法创建文件选择窗口。", L"HardLinkRemover", MB_OK | MB_ICONERROR);
            return false;
        }

        const bool owner_was_enabled = IsWindowEnabled(owner_) != FALSE;
        if (owner_was_enabled)
        {
            EnableWindow(owner_, FALSE);
        }

        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);

        bool received_quit = false;
        int quit_code = 0;
        MSG message{};
        while (window_ != nullptr)
        {
            const BOOL message_result = GetMessageW(&message, nullptr, 0, 0);
            if (message_result == -1)
            {
                break;
            }
            if (message_result == 0)
            {
                received_quit = true;
                quit_code = static_cast<int>(message.wParam);
                break;
            }
            if (IsDialogMessageW(window_, &message) == FALSE)
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        if (window_ != nullptr)
        {
            DestroyWindow(window_);
        }
        if (owner_was_enabled)
        {
            EnableWindow(owner_, TRUE);
            SetForegroundWindow(owner_);
        }
        if (received_quit)
        {
            PostQuitMessage(quit_code);
        }

        if (accepted_)
        {
            selected_paths = std::move(selected_paths_);
        }
        return accepted_;
    }

private:
    static LRESULT CALLBACK window_procedure(
        const HWND window,
        const UINT message,
        const WPARAM w_param,
        const LPARAM l_param)
    {
        SelectionWindow *selection_window = reinterpret_cast<SelectionWindow *>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto *create = reinterpret_cast<const CREATESTRUCTW *>(l_param);
            selection_window = static_cast<SelectionWindow *>(create->lpCreateParams);
            selection_window->window_ = window;
            SetWindowLongPtrW(
                window,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(selection_window));
        }

        if (selection_window != nullptr)
        {
            return selection_window->handle_message(message, w_param, l_param);
        }
        return DefWindowProcW(window, message, w_param, l_param);
    }

    LRESULT handle_message(
        const UINT message,
        const WPARAM w_param,
        const LPARAM l_param)
    {
        switch (message)
        {
        case WM_CREATE:
            dpi_ = window_dpi(window_);
            if (!create_controls() || !create_browser())
            {
                return -1;
            }
            layout_controls();
            return 0;

        case WM_SIZE:
            layout_controls();
            return 0;

        case WM_GETMINMAXINFO:
        {
            auto *information = reinterpret_cast<MINMAXINFO *>(l_param);
            information->ptMinTrackSize.x = scale(720);
            information->ptMinTrackSize.y = scale(480);
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
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(w_param))
            {
            case control_back:
                navigate_history(SBSP_NAVIGATEBACK);
                return 0;
            case control_forward:
                navigate_history(SBSP_NAVIGATEFORWARD);
                return 0;
            case control_parent:
                navigate_to_parent();
                return 0;
            case IDOK:
                accept_selection();
                return 0;
            case IDCANCEL:
                DestroyWindow(window_);
                return 0;
            default:
                break;
            }
            break;

        case WM_CTLCOLORSTATIC:
        {
            const HDC device_context = reinterpret_cast<HDC>(w_param);
            SetTextColor(device_context, GetSysColor(COLOR_WINDOWTEXT));
            SetBkColor(device_context, GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }

        case WM_CLOSE:
            DestroyWindow(window_);
            return 0;

        case WM_DESTROY:
            release_resources();
            return 0;

        case WM_NCDESTROY:
        {
            const LRESULT result = DefWindowProcW(window_, message, w_param, l_param);
            SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
            window_ = nullptr;
            return result;
        }

        default:
            break;
        }
        return DefWindowProcW(window_, message, w_param, l_param);
    }

    bool register_window_class() const
    {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = &SelectionWindow::window_procedure;
        window_class.hInstance = instance_;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = selection_window_class_name;

        if (RegisterClassExW(&window_class) != 0)
        {
            return true;
        }
        return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    HWND create_button(const wchar_t *text, const int identifier, const DWORD style) const
    {
        return CreateWindowExW(
            0,
            L"BUTTON",
            text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
            0,
            0,
            0,
            0,
            window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
            instance_,
            nullptr);
    }

    bool create_controls()
    {
        instruction_ = CreateWindowExW(
            0,
            L"STATIC",
            L"选择文件或文件夹；按住 Ctrl 或 Shift 可多选，双击文件夹可进入。",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0,
            0,
            0,
            0,
            window_,
            nullptr,
            instance_,
            nullptr);
        back_button_ = create_button(L"后退", control_back, BS_PUSHBUTTON);
        forward_button_ = create_button(L"前进", control_forward, BS_PUSHBUTTON);
        parent_button_ = create_button(L"上一级", control_parent, BS_PUSHBUTTON);
        ok_button_ = create_button(L"确定", IDOK, BS_DEFPUSHBUTTON);
        cancel_button_ = create_button(L"取消", IDCANCEL, BS_PUSHBUTTON);

        if (instruction_ == nullptr || back_button_ == nullptr || forward_button_ == nullptr ||
            parent_button_ == nullptr || ok_button_ == nullptr || cancel_button_ == nullptr)
        {
            MessageBoxW(window_, L"无法创建文件选择控件。", L"HardLinkRemover", MB_OK | MB_ICONERROR);
            return false;
        }

        recreate_font();
        return true;
    }

    bool create_browser()
    {
        HRESULT result = CoCreateInstance(
            CLSID_ExplorerBrowser,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&browser_));
        if (FAILED(result) || browser_ == nullptr)
        {
            MessageBoxW(window_, L"无法创建 Explorer 文件视图。", L"HardLinkRemover", MB_OK | MB_ICONERROR);
            return false;
        }

        const auto browser_options = static_cast<EXPLORER_BROWSER_OPTIONS>(
            EBO_SHOWFRAMES | EBO_NOBORDER);
        result = browser_->SetOptions(browser_options);
        if (FAILED(result))
        {
            MessageBoxW(window_, L"无法配置 Explorer 文件视图。", L"HardLinkRemover", MB_OK | MB_ICONERROR);
            return false;
        }

        // 不强制显示项目复选框，完全遵循当前用户的 Explorer 设置。
        SHELLSTATE shell_state{};
        SHGetSetSettings(&shell_state, SSF_AUTOCHECKSELECT, FALSE);

        FOLDERSETTINGS settings{};
        settings.ViewMode = static_cast<UINT>(FVM_AUTO);
        settings.fFlags = shell_state.fAutoCheckSelect != FALSE
                              ? FWF_CHECKSELECT
                              : FWF_NONE;
        const RECT initial_rectangle{0, 0, 1, 1};
        result = browser_->Initialize(window_, &initial_rectangle, &settings);
        if (FAILED(result))
        {
            MessageBoxW(window_, L"无法初始化 Explorer 文件视图。", L"HardLinkRemover", MB_OK | MB_ICONERROR);
            return false;
        }
        browser_initialized_ = true;

        IShellItem *desktop = nullptr;
        result = SHGetKnownFolderItem(
            FOLDERID_Desktop,
            KF_FLAG_DEFAULT,
            nullptr,
            IID_PPV_ARGS(&desktop));
        if (SUCCEEDED(result) && desktop != nullptr)
        {
            result = browser_->BrowseToObject(desktop, SBSP_ABSOLUTE);
            desktop->Release();
        }
        if (FAILED(result))
        {
            MessageBoxW(window_, L"无法打开初始文件夹。", L"HardLinkRemover", MB_OK | MB_ICONERROR);
            return false;
        }
        return true;
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
            back_button_,
            forward_button_,
            parent_button_,
            ok_button_,
            cancel_button_};
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

    void layout_controls() const
    {
        if (window_ == nullptr)
        {
            return;
        }

        RECT client_rectangle{};
        GetClientRect(window_, &client_rectangle);
        const int width = client_rectangle.right;
        const int height = client_rectangle.bottom;
        const int margin = scale(12);
        const int gap = scale(8);
        const int instruction_height = scale(24);
        const int button_height = scale(32);
        const int navigation_width = scale(80);

        MoveWindow(
            instruction_,
            margin,
            margin,
            std::max(0, width - margin * 2),
            instruction_height,
            TRUE);

        const int navigation_top = margin + instruction_height + scale(4);
        MoveWindow(back_button_, margin, navigation_top, navigation_width, button_height, TRUE);
        MoveWindow(
            forward_button_,
            margin + navigation_width + gap,
            navigation_top,
            navigation_width,
            button_height,
            TRUE);
        MoveWindow(
            parent_button_,
            margin + (navigation_width + gap) * 2,
            navigation_top,
            navigation_width,
            button_height,
            TRUE);

        const int action_top = std::max(navigation_top + button_height, height - margin - button_height);
        const int action_width = scale(96);
        MoveWindow(
            cancel_button_,
            std::max(margin, width - margin - action_width),
            action_top,
            action_width,
            button_height,
            TRUE);
        MoveWindow(
            ok_button_,
            std::max(margin, width - margin - action_width * 2 - gap),
            action_top,
            action_width,
            button_height,
            TRUE);

        if (browser_ != nullptr && browser_initialized_)
        {
            const int browser_top = navigation_top + button_height + gap;
            const int browser_bottom = std::max(browser_top, action_top - gap);
            const RECT browser_rectangle{
                margin,
                browser_top,
                std::max(margin, width - margin),
                browser_bottom};
            browser_->SetRect(nullptr, browser_rectangle);
        }
    }

    void navigate_history(const UINT direction) const
    {
        if (browser_ != nullptr)
        {
            browser_->BrowseToIDList(nullptr, direction);
        }
    }

    void navigate_to_parent() const
    {
        if (browser_ == nullptr)
        {
            return;
        }

        IFolderView2 *view = nullptr;
        HRESULT result = browser_->GetCurrentView(IID_PPV_ARGS(&view));
        if (FAILED(result) || view == nullptr)
        {
            return;
        }

        IShellItem *current_folder = nullptr;
        result = view->GetFolder(IID_PPV_ARGS(&current_folder));
        view->Release();
        if (FAILED(result) || current_folder == nullptr)
        {
            return;
        }

        IShellItem *parent = nullptr;
        result = current_folder->GetParent(&parent);
        current_folder->Release();
        if (SUCCEEDED(result) && parent != nullptr)
        {
            browser_->BrowseToObject(parent, SBSP_ABSOLUTE);
            parent->Release();
        }
    }

    void accept_selection()
    {
        if (browser_ == nullptr)
        {
            return;
        }

        IFolderView2 *view = nullptr;
        HRESULT result = browser_->GetCurrentView(IID_PPV_ARGS(&view));
        if (FAILED(result) || view == nullptr)
        {
            MessageBoxW(window_, L"无法读取当前文件视图。", L"HardLinkRemover", MB_OK | MB_ICONERROR);
            return;
        }

        IShellItemArray *selection = nullptr;
        result = view->GetSelection(FALSE, &selection);
        view->Release();
        if (FAILED(result) || selection == nullptr)
        {
            MessageBoxW(window_, L"请先选择至少一个文件或文件夹。", L"HardLinkRemover", MB_OK | MB_ICONINFORMATION);
            return;
        }

        selected_paths_.clear();
        DWORD item_count = 0;
        result = selection->GetCount(&item_count);
        if (SUCCEEDED(result))
        {
            for (DWORD index = 0; index < item_count; ++index)
            {
                IShellItem *item = nullptr;
                if (FAILED(selection->GetItemAt(index, &item)) || item == nullptr)
                {
                    continue;
                }

                SFGAOF attributes = 0;
                const HRESULT attributes_result = item->GetAttributes(SFGAO_FILESYSTEM, &attributes);
                PWSTR path_text = nullptr;
                const HRESULT path_result =
                    SUCCEEDED(attributes_result) && (attributes & SFGAO_FILESYSTEM) != 0
                        ? item->GetDisplayName(SIGDN_FILESYSPATH, &path_text)
                        : E_FAIL;
                item->Release();

                if (SUCCEEDED(path_result) && path_text != nullptr)
                {
                    const std::filesystem::path path(path_text);
                    CoTaskMemFree(path_text);
                    const bool duplicate = std::any_of(
                        selected_paths_.begin(),
                        selected_paths_.end(),
                        [&path](const std::filesystem::path &existing)
                        {
                            return paths_equal(existing, path);
                        });
                    if (!duplicate)
                    {
                        selected_paths_.push_back(path);
                    }
                }
                else if (path_text != nullptr)
                {
                    CoTaskMemFree(path_text);
                }
            }
        }
        selection->Release();

        if (selected_paths_.empty())
        {
            MessageBoxW(
                window_,
                L"所选项目没有可用的文件系统路径，请重新选择文件或文件夹。",
                L"HardLinkRemover",
                MB_OK | MB_ICONINFORMATION);
            return;
        }

        accepted_ = true;
        DestroyWindow(window_);
    }

    void release_resources()
    {
        if (browser_ != nullptr)
        {
            if (browser_initialized_)
            {
                browser_->Destroy();
            }
            browser_->Release();
            browser_ = nullptr;
            browser_initialized_ = false;
        }
        if (font_ != nullptr && owns_font_)
        {
            DeleteObject(font_);
        }
        font_ = nullptr;
        owns_font_ = false;
    }

    HINSTANCE instance_ = nullptr;
    HWND owner_ = nullptr;
    HWND window_ = nullptr;
    HWND instruction_ = nullptr;
    HWND back_button_ = nullptr;
    HWND forward_button_ = nullptr;
    HWND parent_button_ = nullptr;
    HWND ok_button_ = nullptr;
    HWND cancel_button_ = nullptr;
    HFONT font_ = nullptr;
    bool owns_font_ = false;
    UINT dpi_ = default_dpi;
    IExplorerBrowser *browser_ = nullptr;
    bool browser_initialized_ = false;
    bool accepted_ = false;
    std::vector<std::filesystem::path> selected_paths_;
};

} // namespace

bool choose_files_and_folders(
    const HINSTANCE instance,
    const HWND owner,
    std::vector<std::filesystem::path> &selected_paths)
{
    SelectionWindow selection_window;
    return selection_window.show(instance, owner, selected_paths);
}

} // namespace hardlink_remover
