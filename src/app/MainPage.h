#pragma once

#include <filesystem>
#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>

namespace winrt::Unfurl {

struct MainPage : Microsoft::UI::Xaml::Controls::PageT<MainPage> {
    MainPage();

    void LoadPath(winrt::hstring const& path);

  private:
    void BuildUi();
    void SetStatus(winrt::hstring const& text, bool error = false);
    void SetBusy(bool busy);
    void LoadPaths(std::vector<std::filesystem::path> paths);
    winrt::fire_and_forget HandleDrop(winrt::Windows::Foundation::IInspectable const& sender,
                                      winrt::Microsoft::UI::Xaml::DragEventArgs const& args);
    void HandleDragOver(winrt::Windows::Foundation::IInspectable const& sender,
                        winrt::Microsoft::UI::Xaml::DragEventArgs const& args);
    void OnExtract(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnCompress(winrt::Windows::Foundation::IInspectable const&,
                    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);

    Microsoft::UI::Dispatching::DispatcherQueue dispatcher_{nullptr};
    Microsoft::UI::Xaml::Controls::Border drop_zone_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock title_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock subtitle_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock status_{nullptr};
    Microsoft::UI::Xaml::Controls::ListView items_{nullptr};
    Microsoft::UI::Xaml::Controls::ProgressBar progress_{nullptr};
    Microsoft::UI::Xaml::Controls::ComboBox format_{nullptr};
    Microsoft::UI::Xaml::Controls::PasswordBox password_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBox split_size_{nullptr};
    Microsoft::UI::Xaml::Controls::Button extract_{nullptr};
    Microsoft::UI::Xaml::Controls::Button compress_{nullptr};
    std::vector<std::filesystem::path> selected_paths_;
    std::filesystem::path selected_archive_;
    winrt::event_token drop_token_{};
    winrt::event_token drag_over_token_{};
    winrt::event_token extract_token_{};
    winrt::event_token compress_token_{};
};

} // namespace winrt::Unfurl
