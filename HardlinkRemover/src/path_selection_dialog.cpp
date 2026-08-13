#include "path_selection_dialog.h"

#include <commctrl.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <utility>

namespace hardlink_remover
{
namespace
{

constexpr DWORD control_add_selection = 3001;
constexpr HRESULT selection_confirmed = MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_ITF, 1);
constexpr UINT_PTR action_buttons_subclass_id = 1;
constexpr wchar_t unified_add_button_text[] = L"选择";

struct ButtonSearch
{
    HWND button = nullptr;
};

BOOL CALLBACK find_unified_add_button(const HWND window, const LPARAM parameter)
{
    auto *search = reinterpret_cast<ButtonSearch *>(parameter);
    wchar_t text[64]{};
    GetWindowTextW(window, text, static_cast<int>(std::size(text)));
    if (CompareStringOrdinal(text, -1, unified_add_button_text, -1, FALSE) == CSTR_EQUAL)
    {
        search->button = window;
        return FALSE;
    }
    return TRUE;
}

void arrange_action_buttons(const HWND window)
{
    if (window == nullptr)
    {
        return;
    }

    const HWND ok_button = GetDlgItem(window, IDOK);
    if (ok_button == nullptr)
    {
        return;
    }

    ButtonSearch search;
    EnumChildWindows(
        window,
        find_unified_add_button,
        reinterpret_cast<LPARAM>(&search));
    if (search.button != nullptr)
    {
        RECT ok_rectangle{};
        RECT unified_rectangle{};
        if (GetWindowRect(ok_button, &ok_rectangle) != FALSE &&
            GetWindowRect(search.button, &unified_rectangle) != FALSE)
        {
            const int width = unified_rectangle.right - unified_rectangle.left;
            const HWND parent = GetParent(search.button);
            POINT position{ok_rectangle.right - width, ok_rectangle.top};
            ScreenToClient(parent, &position);
            SetWindowPos(
                search.button,
                nullptr,
                position.x,
                position.y,
                width,
                ok_rectangle.bottom - ok_rectangle.top,
                SWP_NOACTIVATE | SWP_NOZORDER);
        }
    }

    EnableWindow(ok_button, FALSE);
    ShowWindow(ok_button, SW_HIDE);
}

LRESULT CALLBACK action_buttons_subclass(
    const HWND window,
    const UINT message,
    const WPARAM w_param,
    const LPARAM l_param,
    const UINT_PTR subclass_id,
    const DWORD_PTR)
{
    const LRESULT result = DefSubclassProc(window, message, w_param, l_param);
    if (message == WM_SIZE || message == WM_DPICHANGED)
    {
        arrange_action_buttons(window);
    }
    else if (message == WM_NCDESTROY)
    {
        RemoveWindowSubclass(window, action_buttons_subclass, subclass_id);
    }
    return result;
}

void arrange_action_buttons(IFileDialog *dialog)
{
    if (dialog == nullptr)
    {
        return;
    }

    IOleWindow *dialog_window = nullptr;
    if (FAILED(dialog->QueryInterface(IID_PPV_ARGS(&dialog_window))) || dialog_window == nullptr)
    {
        return;
    }

    HWND window = nullptr;
    if (SUCCEEDED(dialog_window->GetWindow(&window)) && window != nullptr)
    {
        SetWindowSubclass(
            window,
            action_buttons_subclass,
            action_buttons_subclass_id,
            0);
        arrange_action_buttons(window);
    }
    dialog_window->Release();
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

void append_unique_path(
    std::vector<std::filesystem::path> &paths,
    const std::filesystem::path &candidate)
{
    const bool duplicate = std::any_of(
        paths.begin(),
        paths.end(),
        [&candidate](const std::filesystem::path &existing)
        {
            return paths_equal(existing, candidate);
        });
    if (!duplicate)
    {
        paths.push_back(candidate);
    }
}

bool append_shell_item_path(
    IShellItem *item,
    std::vector<std::filesystem::path> &paths)
{
    if (item == nullptr)
    {
        return false;
    }

    SFGAOF attributes = 0;
    HRESULT result = item->GetAttributes(SFGAO_FILESYSTEM, &attributes);
    if (FAILED(result) || (attributes & SFGAO_FILESYSTEM) == 0)
    {
        return false;
    }

    PWSTR path_text = nullptr;
    result = item->GetDisplayName(SIGDN_FILESYSPATH, &path_text);
    if (FAILED(result) || path_text == nullptr)
    {
        return false;
    }

    append_unique_path(paths, std::filesystem::path(path_text));
    CoTaskMemFree(path_text);
    return true;
}

bool collect_selected_paths(
    IFileOpenDialog *dialog,
    std::vector<std::filesystem::path> &paths)
{
    paths.clear();

    IShellItemArray *selection = nullptr;
    HRESULT result = dialog->GetSelectedItems(&selection);
    if (FAILED(result) || selection == nullptr)
    {
        return false;
    }

    DWORD item_count = 0;
    result = selection->GetCount(&item_count);
    if (SUCCEEDED(result))
    {
        for (DWORD index = 0; index < item_count; ++index)
        {
            IShellItem *item = nullptr;
            if (SUCCEEDED(selection->GetItemAt(index, &item)) && item != nullptr)
            {
                append_shell_item_path(item, paths);
                item->Release();
            }
        }
    }
    selection->Release();
    return !paths.empty();
}

class DialogEvents final : public IFileDialogEvents, public IFileDialogControlEvents
{
public:
    explicit DialogEvents(std::vector<std::filesystem::path> &selected_paths)
        : selected_paths_(selected_paths)
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(
        const IID &interface_id,
        void **object) override
    {
        if (object == nullptr)
        {
            return E_POINTER;
        }

        *object = nullptr;
        if (interface_id == IID_IUnknown || interface_id == IID_IFileDialogEvents)
        {
            *object = static_cast<IFileDialogEvents *>(this);
        }
        else if (interface_id == IID_IFileDialogControlEvents)
        {
            *object = static_cast<IFileDialogControlEvents *>(this);
        }
        else
        {
            return E_NOINTERFACE;
        }

        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++reference_count_;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --reference_count_;
        if (remaining == 0)
        {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog *dialog) override
    {
        IFileOpenDialog *open_dialog = nullptr;
        const HRESULT result = dialog->QueryInterface(IID_PPV_ARGS(&open_dialog));
        if (FAILED(result) || open_dialog == nullptr)
        {
            return S_FALSE;
        }

        const bool collected = collect_selected_paths(open_dialog, selected_paths_);
        open_dialog->Release();
        if (!collected)
        {
            MessageBoxW(
                nullptr,
                L"请先选择至少一个文件或文件夹。",
                L"HardLinkRemover",
                MB_OK | MB_ICONINFORMATION);
            return S_FALSE;
        }

        accepted_ = true;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog *, IShellItem *) override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog *dialog) override
    {
        arrange_action_buttons(dialog);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog *dialog) override
    {
        arrange_action_buttons(dialog);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnShareViolation(
        IFileDialog *,
        IShellItem *,
        FDE_SHAREVIOLATION_RESPONSE *response) override
    {
        if (response != nullptr)
        {
            *response = FDESVR_DEFAULT;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog *) override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnOverwrite(
        IFileDialog *,
        IShellItem *,
        FDE_OVERWRITE_RESPONSE *response) override
    {
        if (response != nullptr)
        {
            *response = FDEOR_DEFAULT;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnItemSelected(
        IFileDialogCustomize *,
        DWORD,
        DWORD) override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnButtonClicked(
        IFileDialogCustomize *customize,
        const DWORD control_id) override
    {
        if (control_id != control_add_selection || customize == nullptr)
        {
            return S_OK;
        }

        IFileOpenDialog *dialog = nullptr;
        HRESULT result = customize->QueryInterface(IID_PPV_ARGS(&dialog));
        if (FAILED(result) || dialog == nullptr)
        {
            return S_OK;
        }

        if (!collect_selected_paths(dialog, selected_paths_))
        {
            MessageBoxW(
                nullptr,
                L"请先选择至少一个文件或文件夹。",
                L"HardLinkRemover",
                MB_OK | MB_ICONINFORMATION);
            dialog->Release();
            return S_OK;
        }

        accepted_ = true;
        dialog->Close(selection_confirmed);
        dialog->Release();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnCheckButtonToggled(
        IFileDialogCustomize *,
        DWORD,
        BOOL) override
    {
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnControlActivating(
        IFileDialogCustomize *,
        DWORD) override
    {
        return S_OK;
    }

    [[nodiscard]] bool accepted() const noexcept
    {
        return accepted_;
    }

private:
    ~DialogEvents() = default;

    std::atomic<ULONG> reference_count_{1};
    std::vector<std::filesystem::path> &selected_paths_;
    bool accepted_ = false;
};

} // namespace

bool choose_files_and_folders(
    const HINSTANCE,
    const HWND owner,
    std::vector<std::filesystem::path> &selected_paths)
{
    selected_paths.clear();

    IFileOpenDialog *dialog = nullptr;
    HRESULT result = CoCreateInstance(
        CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&dialog));
    if (FAILED(result) || dialog == nullptr)
    {
        MessageBoxW(owner, L"无法创建 Windows 文件选择器。", L"HardLinkRemover", MB_OK | MB_ICONERROR);
        return false;
    }

    DWORD options = 0;
    result = dialog->GetOptions(&options);
    if (SUCCEEDED(result))
    {
        result = dialog->SetOptions(
            options | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM |
            FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST | FOS_DONTADDTORECENT);
    }
    if (FAILED(result))
    {
        dialog->Release();
        MessageBoxW(owner, L"无法配置 Windows 文件选择器。", L"HardLinkRemover", MB_OK | MB_ICONERROR);
        return false;
    }

    dialog->SetTitle(L"选择文件或文件夹");
    dialog->SetFileNameLabel(L"文件名");

    IFileDialogCustomize *customize = nullptr;
    result = dialog->QueryInterface(IID_PPV_ARGS(&customize));
    if (FAILED(result) || customize == nullptr)
    {
        dialog->Release();
        MessageBoxW(owner, L"无法配置选择器操作按钮。", L"HardLinkRemover", MB_OK | MB_ICONERROR);
        return false;
    }
    // 用一个按钮统一确认文件、文件夹以及两者的混合选择。
    result = customize->AddPushButton(control_add_selection, unified_add_button_text);
    if (SUCCEEDED(result))
    {
        customize->MakeProminent(control_add_selection);
    }
    customize->Release();
    if (FAILED(result))
    {
        dialog->Release();
        MessageBoxW(owner, L"无法添加混合选择按钮。", L"HardLinkRemover", MB_OK | MB_ICONERROR);
        return false;
    }

    auto *events = new DialogEvents(selected_paths);
    DWORD event_cookie = 0;
    result = dialog->Advise(events, &event_cookie);
    if (FAILED(result))
    {
        events->Release();
        dialog->Release();
        MessageBoxW(owner, L"无法初始化文件选择器事件。", L"HardLinkRemover", MB_OK | MB_ICONERROR);
        return false;
    }

    result = dialog->Show(owner);
    const bool accepted = events->accepted() &&
                          (SUCCEEDED(result) || result == selection_confirmed);

    dialog->Unadvise(event_cookie);
    events->Release();
    dialog->Release();

    if (!accepted)
    {
        selected_paths.clear();
    }
    return accepted;
}

} // namespace hardlink_remover
