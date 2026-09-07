#include "App.h"

#include <Windows.h>
#include <microsoft.ui.xaml.window.h>
#include <shellapi.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace {

std::pair<std::vector<std::filesystem::path>, bool> launch_paths() {
    int count{};
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments)
        throw_last_error();
    std::vector<std::filesystem::path> paths;
    bool compress{};
    for (int i = 1; i < count; ++i) {
        if (std::wstring_view(arguments[i]) == L"--compress")
            compress = true;
        else
            paths.emplace_back(arguments[i]);
    }
    LocalFree(arguments);
    return {std::move(paths), compress};
}

} // namespace

namespace winrt::Unfurl {

App::App() {
    metadata_ = Microsoft::UI::Xaml::XamlTypeInfo::XamlControlsXamlMetaDataProvider();
}

Microsoft::UI::Xaml::Markup::IXamlType App::GetXamlType(Windows::UI::Xaml::Interop::TypeName const& type) {
    return metadata_.GetXamlType(type);
}

Microsoft::UI::Xaml::Markup::IXamlType App::GetXamlType(hstring const& name) {
    return metadata_.GetXamlType(name);
}

com_array<Microsoft::UI::Xaml::Markup::XmlnsDefinition> App::GetXmlnsDefinitions() {
    return metadata_.GetXmlnsDefinitions();
}

void App::OnLaunched(LaunchActivatedEventArgs const&) {
    // Programmatic WinUI applications must supply the Fluent resources after activation.
    auto resources = ResourceDictionary();
    resources.MergedDictionaries().Append(Microsoft::UI::Xaml::Controls::XamlControlsResources());
    Resources(resources);
    window_ = Window();
    window_.Title(L"Unfurl");
    // Extend Mica into the title area; Windows still owns the caption buttons and snap behavior.
    window_.ExtendsContentIntoTitleBar(true);
    window_.SystemBackdrop(Microsoft::UI::Xaml::Media::MicaBackdrop());
    auto page = winrt::make_self<MainPage>();
    auto page_element = page.as<Microsoft::UI::Xaml::Controls::Page>();
    window_.Content(page_element);
    HWND hwnd{};
    check_hresult(window_.as<IWindowNative>()->get_WindowHandle(&hwnd));
    const auto dpi = GetDpiForWindow(hwnd);
    page->InitializeWindow(window_, reinterpret_cast<std::uintptr_t>(hwnd));
    MONITORINFO monitor{sizeof(MONITORINFO)};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
    const auto work = monitor.rcWork;
    const auto width = std::min(MulDiv(960, dpi, 96), static_cast<int>(work.right - work.left - 32));
    const auto height = std::min(MulDiv(660, dpi, 96), static_cast<int>(work.bottom - work.top - 32));
    auto app_window = window_.AppWindow();
    app_window.MoveAndResize(Windows::Graphics::RectInt32{work.left + (work.right - work.left - width) / 2,
                                                          work.top + (work.bottom - work.top - height) / 2, width,
                                                          height});
    auto presenter = app_window.Presenter().as<Microsoft::UI::Windowing::OverlappedPresenter>();
    presenter.PreferredMinimumWidth(box_value(MulDiv(560, dpi, 96)).as<Windows::Foundation::IReference<int>>());
    presenter.PreferredMinimumHeight(box_value(MulDiv(480, dpi, 96)).as<Windows::Foundation::IReference<int>>());
    const auto small_icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                          GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                                                          GetSystemMetricsForDpi(SM_CYSMICON, dpi), 0));
    const auto large_icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                          GetSystemMetricsForDpi(SM_CXICON, dpi),
                                                          GetSystemMetricsForDpi(SM_CYICON, dpi), 0));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large_icon));
    window_.Closed([small_icon, large_icon](auto&&, auto&&) {
        DestroyIcon(small_icon);
        DestroyIcon(large_icon);
    });
    app_window.Closing(
        [page, window = window_](auto&&, Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args) {
            if (page->Busy()) {
                args.Cancel(true);
                page->RequestClose([window] { window.Close(); });
            }
        });
    window_.Activate();
    auto [paths, compress] = launch_paths();
    page->LoadPaths(std::move(paths), compress);
}

} // namespace winrt::Unfurl
