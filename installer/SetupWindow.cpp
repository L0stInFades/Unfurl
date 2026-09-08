#include "SetupDeployment.h"

#include <microsoft.ui.xaml.window.h>
#include <shellapi.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.UI.Xaml.Interop.h>

namespace unfurl::setup {
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

struct SetupApp : ApplicationT<SetupApp, Markup::IXamlMetadataProvider> {
    explicit SetupApp(std::shared_ptr<Session> session) : session_(std::move(session)) {
    }
    Markup::IXamlType GetXamlType(Windows::UI::Xaml::Interop::TypeName const& type) {
        return metadata_.GetXamlType(type);
    }
    Markup::IXamlType GetXamlType(hstring const& type) {
        return metadata_.GetXamlType(type);
    }
    com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() {
        return metadata_.GetXmlnsDefinitions();
    }

    void OnLaunched(LaunchActivatedEventArgs const&) {
        Resources().MergedDictionaries().Append(XamlControlsResources());
        const auto module = GetModuleHandleW(nullptr);
        const auto resource = FindResourceW(module, MAKEINTRESOURCEW(13), RT_RCDATA);
        if (!resource)
            throw_last_error();
        const auto data = LockResource(LoadResource(module, resource));
        const auto root = Markup::XamlReader::Load(to_hstring(std::string_view(static_cast<char const*>(data),
                                                                               SizeofResource(module, resource))))
                              .as<Grid>();
        start_ = root.FindName(L"ContinueSetup").as<Button>();
        cancel_ = root.FindName(L"CancelSetup").as<Button>();
        status_ = root.FindName(L"SetupStatus").as<TextBlock>();
        progress_ = root.FindName(L"SetupProgress").as<ProgressBar>();
        window_ = Window();
        window_.Title(L"Unfurl 安装");
        window_.Content(root);
        window_.ExtendsContentIntoTitleBar(true);
        window_.SetTitleBar(root.FindName(L"SetupTitleBar").as<TitleBar>());
        window_.SystemBackdrop(Media::MicaBackdrop());
        auto app_window = window_.AppWindow();
        HWND handle{};
        check_hresult(window_.as<IWindowNative>()->get_WindowHandle(&handle));
        const auto dpi = GetDpiForWindow(handle);
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor))
            throw_last_error();
        const auto width = MulDiv(540, dpi, 96), height = MulDiv(420, dpi, 96);
        app_window.MoveAndResize(Windows::Graphics::RectInt32{
            monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2,
            monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2, width, height});
        const auto presenter = app_window.Presenter().as<Microsoft::UI::Windowing::OverlappedPresenter>();
        presenter.IsResizable(false);
        presenter.IsMaximizable(false);
        icon_ = static_cast<HICON>(LoadImageW(module, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                              GetSystemMetricsForDpi(SM_CXICON, dpi),
                                              GetSystemMetricsForDpi(SM_CYICON, dpi), 0));
        SendMessageW(handle, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_));
        const auto caption = app_window.TitleBar();
        const auto update_theme = [caption, root] {
            caption.ButtonBackgroundColor(Windows::UI::Color{0, 0, 0, 0});
            caption.ButtonInactiveBackgroundColor(Windows::UI::Color{0, 0, 0, 0});
            caption.PreferredTheme(root.ActualTheme() == ElementTheme::Dark
                                       ? Microsoft::UI::Windowing::TitleBarTheme::Dark
                                       : Microsoft::UI::Windowing::TitleBarTheme::Light);
        };
        root.ActualThemeChanged([update_theme](auto&&, auto&&) { update_theme(); });
        update_theme();
        start_.Click([this](auto&&, auto&&) {
            if (finished_)
                window_.Close();
            else
                Begin();
        });
        cancel_.Click([this](auto&&, auto&&) {
            if (!busy_)
                window_.Close();
            else {
                session_->cancelled.store(true);
                cancel_.IsEnabled(false);
                SetStatus(L"正在取消并清理…");
            }
        });
        app_window.Closing([this](auto&&, Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args) {
            if (busy_) {
                args.Cancel(true);
                close_when_idle_ = true;
                session_->cancelled.store(true);
                SetStatus(L"正在取消并清理…");
            }
        });
        window_.Closed([this](auto&&, auto&&) {
            if (icon_)
                DestroyIcon(std::exchange(icon_, nullptr));
        });
        window_.Activate();
    }

    fire_and_forget Begin() {
        if (busy_)
            co_return;
        const auto lifetime = get_strong();
        const apartment_context ui;
        const auto dispatcher = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        busy_ = true;
        session_->cancelled.store(false);
        start_.IsEnabled(false);
        cancel_.IsEnabled(true);
        progress_.Visibility(Visibility::Visible);
        progress_.IsIndeterminate(true);
        SetStatus(L"正在准备安装证书…");
        std::wstring failure;
        co_await resume_background();
        try {
            request_certificate_trust(*session_);
            prepare_dependencies(*session_, [weak = get_weak(), dispatcher](auto const& text) {
                dispatcher.TryEnqueue([weak, text] {
                    if (auto app = weak.get(); app && !app->session_->cancelled.load())
                        app->SetStatus(text);
                });
            });
        } catch (hresult_error const& error) {
            failure = error_text(error);
        } catch (std::exception const&) {
            failure = L"无法完成准备，请检查网络和剩余磁盘空间后重试。";
        }
        co_await ui;
        busy_ = false;
        progress_.IsIndeterminate(false);
        progress_.Visibility(Visibility::Collapsed);
        start_.IsEnabled(true);
        cancel_.IsEnabled(true);
        if (close_when_idle_) {
            window_.Close();
            co_return;
        }
        if (session_->cancelled.load()) {
            SetStatus(L"已取消。可以重新准备安装。");
            co_return;
        }
        if (failure.empty()) {
            try {
                open_app_installer(*session_);
            } catch (hresult_error const& error) {
                failure = error_text(error);
            }
        }
        if (!failure.empty()) {
            SetStatus(failure);
            start_.Content(box_value(L"重试"));
            co_return;
        }
        finished_ = true;
        SetStatus(L"准备完成。请在 Windows 安装窗口中确认安装。\n安装后将自动接收更新。");
        start_.Content(box_value(L"完成"));
        cancel_.Visibility(Visibility::Collapsed);
    }

  private:
    void SetStatus(param::hstring const& text) {
        status_.Text(text);
        using namespace Automation::Peers;
        if (AutomationPeer::ListenerExists(AutomationEvents::LiveRegionChanged)) {
            if (const auto peer = FrameworkElementAutomationPeer::CreatePeerForElement(status_))
                peer.RaiseAutomationEvent(AutomationEvents::LiveRegionChanged);
        }
    }

    XamlTypeInfo::XamlControlsXamlMetaDataProvider metadata_;
    std::shared_ptr<Session> session_;
    Window window_{nullptr};
    Button start_{nullptr}, cancel_{nullptr};
    TextBlock status_{nullptr};
    ProgressBar progress_{nullptr};
    HICON icon_{};
    bool busy_{}, finished_{}, close_when_idle_{};
};

void run_window(std::shared_ptr<Session> session) {
    Application::Start([session = std::move(session)](auto&&) { make<SetupApp>(session); });
}

} // namespace unfurl::setup
