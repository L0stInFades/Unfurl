#include "SetupDeployment.h"

#include <commctrl.h>
#include <shellapi.h>
#include <thread>

namespace unfurl::setup {

int prepare_runtime_dialog(std::shared_ptr<Session> const& session) {
    struct State {
        std::shared_ptr<Session> session;
        std::thread worker;
        std::wstring error;
        std::atomic_bool done{false};
    } state{session};
    TASKDIALOGCONFIG dialog{sizeof(dialog)};
    dialog.hInstance = GetModuleHandleW(nullptr);
    dialog.pszWindowTitle = L"Unfurl 安装";
    dialog.pszMainIcon = MAKEINTRESOURCEW(1);
    dialog.pszMainInstruction = L"正在准备安装界面";
    dialog.pszContent = L"首次使用需要安装微软 Windows App Runtime。准备完成后将打开 Unfurl 安装页。";
    dialog.dwFlags = TDF_SHOW_MARQUEE_PROGRESS_BAR | TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    dialog.lpCallbackData = reinterpret_cast<LONG_PTR>(&state);
    dialog.pfCallback = [](HWND window, UINT notification, WPARAM, LPARAM, LONG_PTR context) -> HRESULT {
        auto& state = *reinterpret_cast<State*>(context);
        if (notification == TDN_CREATED) {
            SendMessageW(window, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 0);
            state.worker = std::thread([&state, window] {
                try {
                    prepare_dependencies(*state.session, [window](auto const& text) {
                        SendMessageW(window, TDM_SET_ELEMENT_TEXT, TDE_CONTENT, reinterpret_cast<LPARAM>(text.c_str()));
                    });
                } catch (winrt::hresult_error const& error) {
                    state.error = error_text(error);
                } catch (std::exception const&) {
                    state.error = L"无法准备 Windows 安装组件。请检查网络和剩余磁盘空间后重试。";
                }
                state.done.store(true, std::memory_order_release);
                PostMessageW(window, TDM_CLICK_BUTTON, IDCANCEL, 0);
            });
        } else if (notification == TDN_BUTTON_CLICKED && !state.done.load(std::memory_order_acquire)) {
            state.session->cancelled.store(true, std::memory_order_relaxed);
            SendMessageW(window, TDM_SET_ELEMENT_TEXT, TDE_MAIN_INSTRUCTION,
                         reinterpret_cast<LPARAM>(L"正在取消并清理…"));
            return S_FALSE;
        }
        return S_OK;
    };
    const auto result = TaskDialogIndirect(&dialog, nullptr, nullptr, nullptr);
    if (state.worker.joinable())
        state.worker.join();
    winrt::check_hresult(result);
    if (session->cancelled.load(std::memory_order_relaxed))
        return ERROR_CANCELLED;
    if (!state.error.empty())
        throw winrt::hresult_error(E_FAIL, state.error);
    return ERROR_SUCCESS;
}

} // namespace unfurl::setup

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR command, int) {
    using namespace unfurl::setup;
    // Elevation performs one fixed action on the embedded public certificate.
    const bool trust = std::wstring_view(command) == L"--trust-certificate";
    try {
        SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS);
        if (trust)
            return trust_certificate();
        if (*command)
            throw winrt::hresult_invalid_argument(L"安装程序参数无效。");
        OSVERSIONINFOEXW version{sizeof(version)};
        version.dwMajorVersion = 10;
        version.dwBuildNumber = 22000;
        // VerifyVersionInfo requires the minor and service-pack fields with the major version.
        constexpr DWORD fields =
            VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR | VER_SERVICEPACKMINOR | VER_BUILDNUMBER;
        DWORDLONG mask{};
        for (const DWORD field :
             {VER_MAJORVERSION, VER_MINORVERSION, VER_SERVICEPACKMAJOR, VER_SERVICEPACKMINOR, VER_BUILDNUMBER})
            mask = VerSetConditionMask(mask, field, VER_GREATER_EQUAL);
        if (!VerifyVersionInfoW(&version, fields, mask))
            throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_OLD_WIN_VERSION),
                                       L"Unfurl 需要 Windows 11 或更新版本。");
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        if (!app_installer_available())
            throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_INSTALL_PACKAGE_NOT_FOUND),
                                       L"请先从 Microsoft Store 安装“应用安装程序”，然后重新运行 Unfurl 安装程序。");
        const auto session = std::make_shared<Session>();
        Bootstrap bootstrap(*session);
        if (FAILED(bootstrap.Initialize())) {
            if (prepare_runtime_dialog(session) == ERROR_CANCELLED)
                return ERROR_CANCELLED;
            winrt::check_hresult(bootstrap.Initialize());
        }
        run_window(session);
        return ERROR_SUCCESS;
    } catch (winrt::hresult_error const& error) {
        if (!trust)
            MessageBoxW(nullptr, error_text(error).c_str(), L"Unfurl 安装", MB_OK | MB_ICONERROR);
        return static_cast<int>(error.code().value);
    } catch (std::exception const&) {
        if (!trust)
            MessageBoxW(nullptr, L"无法启动安装。请检查剩余磁盘空间后重试。", L"Unfurl 安装", MB_OK | MB_ICONERROR);
        return ERROR_INSTALL_FAILURE;
    }
}
