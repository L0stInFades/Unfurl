#pragma once

#include "unfurl/archive_engine.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.h>

namespace winrt::Unfurl {

struct MainPage : Microsoft::UI::Xaml::Controls::PageT<MainPage> {
    MainPage();
    void InitializeWindow(Microsoft::UI::Xaml::Window const& window, std::uintptr_t handle);
    void LoadPaths(std::vector<std::filesystem::path> paths, bool compress = false);
    void RequestClose(std::function<void()> close);
    [[nodiscard]] bool Busy() const;

  private:
    using ArchiveTask = std::function<unfurl::ArchiveResult(const std::function<bool()>&,
                                                            const unfurl::ArchiveEngine::ProgressCallback&)>;
    void BuildUi();
    void SetStatus(hstring const& text, bool error = false, hstring const& detail = {});
    void SetFailure(std::string const& message, std::optional<unfurl::ArchiveFailure::Code> code = {});
    void SetBusy(bool busy);
    void RefreshSelection();
    void RefreshEntrySelection();
    void OnEntrySelectionChanged(Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
    void LoadArchivePreview();
    void RefreshDestination();
    void UpdateCaptionTheme();
    void PrepareOptionsTransition();
    void UpdateOptionsViewport(Microsoft::UI::Xaml::SizeChangedEventArgs const& args);
    void UpdateResponsiveLayout(double width);
    void AddRow(hstring const& path, std::uint64_t size, bool directory);
    void PickFiles(bool archives, bool folders);
    void PickDestination();
    void ClearSelection();
    void ShowOutput();
    void StartArchiveTask(ArchiveTask task, bool extracting);
    void FinishFailure(std::string message, bool cancelled, std::uint64_t operation,
                       std::optional<unfurl::ArchiveFailure::Code> code = {});
    void OnCancel(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnFormatChanged(Windows::Foundation::IInspectable const&,
                         Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void OnExtract(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void OnCompress(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    fire_and_forget HandleDrop(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::DragEventArgs const&);
    void HandleDragOver(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::DragEventArgs const&);

    Microsoft::UI::Dispatching::DispatcherQueue dispatcher_{nullptr};
    Microsoft::UI::Windowing::AppWindowTitleBar title_bar_chrome_{nullptr};
    Microsoft::UI::Xaml::Controls::TitleBar title_bar_{nullptr};
    Microsoft::UI::Xaml::Controls::NavigationView navigation_{nullptr};
    Microsoft::UI::Xaml::Controls::NavigationViewItem compress_navigation_{nullptr};
    Microsoft::UI::Xaml::Controls::NavigationViewItem extract_navigation_{nullptr};
    Microsoft::UI::Xaml::Controls::Grid workspace_{nullptr};
    Microsoft::UI::Xaml::Controls::Grid app_settings_{nullptr};
    Microsoft::UI::Xaml::Controls::Grid list_header_{nullptr};
    Microsoft::UI::Xaml::Controls::ScrollViewer workspace_scroll_{nullptr};
    Microsoft::UI::Xaml::Controls::StackPanel body_{nullptr};
    Microsoft::UI::Xaml::Controls::Grid name_format_{nullptr};
    Microsoft::UI::Xaml::Controls::Grid split_row_{nullptr};
    Microsoft::UI::Xaml::Controls::Expander options_{nullptr};
    Microsoft::UI::Xaml::Controls::StackPanel empty_selection_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock empty_selection_text_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock workspace_heading_{nullptr};
    Microsoft::UI::Xaml::Controls::Border drop_zone_{nullptr};
    Microsoft::UI::Xaml::Controls::StackPanel items_panel_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock selection_title_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock selection_detail_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock items_heading_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock settings_heading_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock status_{nullptr};
    Microsoft::UI::Xaml::Controls::ListView items_{nullptr};
    Microsoft::UI::Xaml::Controls::ProgressBar progress_{nullptr};
    Microsoft::UI::Xaml::Controls::ComboBox format_{nullptr};
    Microsoft::UI::Xaml::Controls::ComboBox theme_{nullptr};
    Microsoft::UI::Xaml::Controls::PasswordBox password_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBox split_size_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBox archive_name_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock destination_{nullptr};
    Microsoft::UI::Xaml::Controls::TextBlock destination_path_{nullptr};
    Microsoft::UI::Xaml::Controls::CheckBox select_all_{nullptr};
    Microsoft::UI::Xaml::Controls::Button read_archive_{nullptr};
    Microsoft::UI::Xaml::Controls::Button open_{nullptr};
    Microsoft::UI::Xaml::Controls::Button add_files_{nullptr};
    Microsoft::UI::Xaml::Controls::Button add_folder_{nullptr};
    Microsoft::UI::Xaml::Controls::Button clear_{nullptr};
    Microsoft::UI::Xaml::Controls::Button choose_destination_{nullptr};
    Microsoft::UI::Xaml::Controls::Button reveal_{nullptr};
    Microsoft::UI::Xaml::Controls::Button extract_{nullptr};
    Microsoft::UI::Xaml::Controls::Button compress_{nullptr};
    Microsoft::UI::Xaml::Controls::Button cancel_{nullptr};
    std::vector<std::filesystem::path> selected_paths_;
    std::vector<std::filesystem::path> selected_archives_;
    struct PreviewEntry {
        unfurl::ArchiveItem item;
        std::size_t archive{};
    };
    std::vector<PreviewEntry> preview_entries_;
    std::vector<bool> entry_selection_;
    std::filesystem::path output_directory_;
    std::filesystem::path last_output_;
    std::shared_ptr<std::atomic_bool> cancellation_;
    std::function<void()> close_when_idle_;
    std::uintptr_t window_handle_{};
    std::uint64_t operation_id_{};
    hstring status_detail_;
    std::optional<double> collapse_scroll_target_;
    double collapsing_options_height_{};
    bool reveal_options_{};
    bool busy_{};
    bool status_error_{};
    bool extract_mode_{};
    bool updating_selection_{};
};

} // namespace winrt::Unfurl
