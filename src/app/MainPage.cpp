#include "MainPage.h"

#include "unfurl/archive_engine.hpp"

#include <Windows.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Text.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace Windows::ApplicationModel::DataTransfer;
using namespace Windows::Storage;

namespace {

Windows::UI::Color color(std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha = 255) {
    return Windows::UI::Color{alpha, red, green, blue};
}

hstring from_utf8(std::string_view value) {
    if (value.empty())
        return {};
    const auto required =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0)
        return hstring(std::wstring(value.begin(), value.end()));
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), wide.data(),
                        required);
    return hstring(wide);
}

SolidColorBrush brush(std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha = 255) {
    return SolidColorBrush(color(red, green, blue, alpha));
}

Thickness thickness(double left, double top, double right, double bottom) {
    Thickness value{};
    value.Left = left;
    value.Top = top;
    value.Right = right;
    value.Bottom = bottom;
    return value;
}

Thickness thickness(double uniform) {
    return thickness(uniform, uniform, uniform, uniform);
}

CornerRadius corner_radius(double top_left, double top_right, double bottom_right, double bottom_left) {
    CornerRadius value{};
    value.TopLeft = top_left;
    value.TopRight = top_right;
    value.BottomRight = bottom_right;
    value.BottomLeft = bottom_left;
    return value;
}

unfurl::ArchiveFormat format_from_index(std::int32_t index) {
    switch (index) {
    case 1:
        return unfurl::ArchiveFormat::seven_zip;
    case 2:
        return unfurl::ArchiveFormat::tar_gzip;
    case 3:
        return unfurl::ArchiveFormat::tar_bzip2;
    case 4:
        return unfurl::ArchiveFormat::tar_xz;
    case 5:
        return unfurl::ArchiveFormat::tar_zstd;
    default:
        return unfurl::ArchiveFormat::zip;
    }
}

} // namespace

namespace winrt::Unfurl {

MainPage::MainPage() {
    dispatcher_ = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    BuildUi();
}

void MainPage::BuildUi() {
    auto root = Grid();
    root.Padding(thickness(32, 24, 32, 28));
    root.RowDefinitions().Append(RowDefinition());
    root.RowDefinitions().Append(RowDefinition());
    root.RowDefinitions().Append(RowDefinition());
    root.RowDefinitions().GetAt(0).Height(GridLength{1, GridUnitType::Auto});
    root.RowDefinitions().GetAt(1).Height(GridLength{1, GridUnitType::Star});
    root.RowDefinitions().GetAt(2).Height(GridLength{1, GridUnitType::Auto});

    auto header = StackPanel();
    header.Spacing(4);
    title_ = TextBlock();
    title_.Text(L"Unfurl");
    title_.FontFamily(Media::FontFamily(L"Segoe UI Variable Display"));
    title_.FontSize(30);
    title_.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
    subtitle_ = TextBlock();
    subtitle_.Text(L"Drop an archive to inspect it, or files to compress them.");
    subtitle_.FontFamily(Media::FontFamily(L"Segoe UI Variable Text"));
    subtitle_.FontSize(14);
    subtitle_.Foreground(brush(100, 100, 100));
    header.Children().Append(title_);
    header.Children().Append(subtitle_);
    Grid::SetRow(header, 0);
    root.Children().Append(header);

    drop_zone_ = Border();
    drop_zone_.Margin(thickness(0, 24, 0, 20));
    drop_zone_.Padding(thickness(24));
    drop_zone_.CornerRadius(corner_radius(12, 12, 12, 12));
    drop_zone_.BorderThickness(thickness(1));
    drop_zone_.BorderBrush(brush(0, 103, 192, 110));
    drop_zone_.Background(brush(0, 103, 192, 18));
    drop_zone_.AllowDrop(true);
    auto drop_content = StackPanel();
    drop_content.HorizontalAlignment(HorizontalAlignment::Center);
    drop_content.VerticalAlignment(VerticalAlignment::Center);
    drop_content.Spacing(8);
    auto glyph = FontIcon();
    glyph.Glyph(L"\xE74D");
    glyph.FontSize(34);
    glyph.HorizontalAlignment(HorizontalAlignment::Center);
    glyph.Foreground(brush(0, 103, 192));
    auto prompt = TextBlock();
    prompt.Text(L"Drop files here");
    prompt.FontFamily(Media::FontFamily(L"Segoe UI Variable Text"));
    prompt.FontSize(18);
    prompt.HorizontalAlignment(HorizontalAlignment::Center);
    drop_content.Children().Append(glyph);
    drop_content.Children().Append(prompt);
    drop_zone_.Child(drop_content);
    drop_zone_.DragOver(DragEventHandler{this, &MainPage::HandleDragOver});
    drop_zone_.Drop(DragEventHandler{this, &MainPage::HandleDrop});
    auto content_area = StackPanel();
    content_area.Spacing(12);
    content_area.Children().Append(drop_zone_);

    auto options = StackPanel();
    options.Orientation(Orientation::Horizontal);
    options.HorizontalAlignment(HorizontalAlignment::Left);
    options.Spacing(12);
    format_ = ComboBox();
    format_.Header(box_value(L"Format"));
    format_.Items().Append(box_value(L"ZIP"));
    format_.Items().Append(box_value(L"7Z"));
    format_.Items().Append(box_value(L"TAR.GZ"));
    format_.Items().Append(box_value(L"TAR.BZ2"));
    format_.Items().Append(box_value(L"TAR.XZ"));
    format_.Items().Append(box_value(L"TAR.ZST"));
    format_.SelectedIndex(0);
    format_.Width(140);
    password_ = PasswordBox();
    password_.Header(box_value(L"Password"));
    password_.PlaceholderText(L"Optional");
    password_.MaxLength(1024);
    password_.Width(190);
    split_size_ = TextBox();
    split_size_.Header(box_value(L"Split size (MB)"));
    split_size_.PlaceholderText(L"ZIP only");
    split_size_.Width(140);
    options.Children().Append(format_);
    options.Children().Append(password_);
    options.Children().Append(split_size_);
    content_area.Children().Append(options);
    items_ = ListView();
    items_.Height(190);
    items_.Visibility(Visibility::Collapsed);
    items_.BorderThickness(thickness(1));
    items_.BorderBrush(brush(120, 120, 120, 70));
    items_.CornerRadius(corner_radius(8, 8, 8, 8));
    content_area.Children().Append(items_);
    Grid::SetRow(content_area, 1);
    root.Children().Append(content_area);

    auto footer = StackPanel();
    footer.Orientation(Orientation::Horizontal);
    footer.HorizontalAlignment(HorizontalAlignment::Stretch);
    footer.Spacing(8);
    status_ = TextBlock();
    status_.Text(L"Ready");
    status_.VerticalAlignment(VerticalAlignment::Center);
    status_.FontFamily(Media::FontFamily(L"Segoe UI Variable Text"));
    progress_ = ProgressBar();
    progress_.Width(180);
    progress_.Height(4);
    progress_.Visibility(Visibility::Collapsed);
    progress_.VerticalAlignment(VerticalAlignment::Center);
    progress_.Margin(thickness(0, 0, 16, 0));
    extract_ = Button();
    extract_.Content(box_value(L"Extract"));
    extract_.Margin(thickness(8, 0, 0, 0));
    extract_.IsEnabled(false);
    compress_ = Button();
    compress_.Content(box_value(L"Compress"));
    compress_.Margin(thickness(8, 0, 0, 0));
    compress_.IsEnabled(false);
    extract_.Click(RoutedEventHandler{this, &MainPage::OnExtract});
    compress_.Click(RoutedEventHandler{this, &MainPage::OnCompress});
    status_.HorizontalAlignment(HorizontalAlignment::Stretch);
    footer.Children().Append(status_);
    footer.Children().Append(progress_);
    footer.Children().Append(extract_);
    footer.Children().Append(compress_);
    Grid::SetRow(footer, 2);
    root.Children().Append(footer);
    Content(root);
}

void MainPage::SetStatus(hstring const& text, bool error) {
    status_.Text(text);
    status_.Foreground(error ? brush(196, 43, 28) : brush(100, 100, 100));
}

void MainPage::SetBusy(bool busy) {
    progress_.Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
    extract_.IsEnabled(!busy && !selected_archive_.empty());
    compress_.IsEnabled(!busy && !selected_paths_.empty() && selected_archive_.empty());
    format_.IsEnabled(!busy);
    password_.IsEnabled(!busy);
    split_size_.IsEnabled(!busy);
    drop_zone_.IsHitTestVisible(!busy);
}

void MainPage::HandleDragOver(Windows::Foundation::IInspectable const&, DragEventArgs const& args) {
    args.AcceptedOperation(DataPackageOperation::Copy);
    args.Handled(true);
}

fire_and_forget MainPage::HandleDrop(Windows::Foundation::IInspectable const&, DragEventArgs const& args) {
    auto lifetime = get_strong();
    const auto deferral = args.GetDeferral();
    try {
        const auto view = args.DataView();
        if (!view.Contains(StandardDataFormats::StorageItems())) {
            deferral.Complete();
            co_return;
        }
        const auto items = co_await view.GetStorageItemsAsync();
        std::vector<std::filesystem::path> paths;
        for (const auto& item : items) {
            if (const auto file = item.try_as<StorageFile>()) {
                paths.emplace_back(file.Path().c_str());
            } else if (const auto folder = item.try_as<StorageFolder>()) {
                paths.emplace_back(folder.Path().c_str());
            }
        }
        LoadPaths(std::move(paths));
    } catch (const hresult_error& error) {
        SetStatus(error.message(), true);
    }
    deferral.Complete();
}

void MainPage::LoadPath(hstring const& path) {
    LoadPaths({std::filesystem::path(path.c_str())});
}

void MainPage::LoadPaths(std::vector<std::filesystem::path> paths) {
    if (paths.empty())
        return;
    selected_paths_.clear();
    selected_archive_.clear();
    password_.Password(L"");
    items_.Items().Clear();
    items_.Visibility(Visibility::Collapsed);
    if (paths.size() == 1 && unfurl::ArchiveEngine::is_archive(paths.front())) {
        selected_archive_ = paths.front();
        SetStatus(L"Reading archive…");
        SetBusy(true);
        const auto source = selected_archive_;
        std::thread([lifetime = get_strong(), source] {
            try {
                const auto preview = unfurl::ArchiveEngine::preview(source);
                lifetime->dispatcher_.TryEnqueue([lifetime, preview] {
                    lifetime->items_.Visibility(Visibility::Visible);
                    for (const auto& item : preview.items) {
                        auto row = TextBlock();
                        row.Text(from_utf8(item.path));
                        row.FontFamily(Media::FontFamily(L"Segoe UI Variable Text"));
                        row.Padding(thickness(4));
                        lifetime->items_.Items().Append(row);
                    }
                    const auto count =
                        std::to_string(preview.items.size()) + (preview.truncated ? "+ items" : " items");
                    lifetime->SetStatus(preview.encrypted ? from_utf8(count + "; enter password to extract")
                                                          : from_utf8(count));
                    lifetime->SetBusy(false);
                });
            } catch (const unfurl::ArchiveFailure& error) {
                lifetime->dispatcher_.TryEnqueue([lifetime, message = std::string(error.what())] {
                    lifetime->SetStatus(from_utf8(message), true);
                    lifetime->SetBusy(false);
                });
            }
        }).detach();
    } else {
        selected_paths_ = std::move(paths);
        SetStatus(from_utf8(std::to_string(selected_paths_.size()) + " items ready to compress"));
        SetBusy(false);
    }
}

void MainPage::OnExtract(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (selected_archive_.empty())
        return;
    const auto source = selected_archive_;
    const auto destination = source.parent_path();
    const auto password = to_string(password_.Password());
    SetBusy(true);
    SetStatus(L"Extracting…");
    std::thread([lifetime = get_strong(), source, destination, password] {
        try {
            std::optional<std::string_view> passphrase;
            if (!password.empty())
                passphrase = password;
            const auto result = unfurl::ArchiveEngine::extract(source, destination, passphrase);
            lifetime->dispatcher_.TryEnqueue([lifetime, output = result.output().wstring()] {
                lifetime->SetStatus(hstring(std::wstring(L"Extracted to ") + output));
                lifetime->SetBusy(false);
            });
        } catch (const unfurl::ArchiveFailure& error) {
            lifetime->dispatcher_.TryEnqueue([lifetime, message = std::string(error.what())] {
                lifetime->SetStatus(from_utf8(message), true);
                lifetime->SetBusy(false);
            });
        }
    }).detach();
}

void MainPage::OnCompress(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (selected_paths_.empty())
        return;
    const auto paths = selected_paths_;
    const auto destination = paths.front().parent_path();
    const auto name = paths.size() == 1 ? paths.front().stem().string() : "Archive";
    const auto password = to_string(password_.Password());
    const auto split_text = to_string(split_size_.Text());
    std::optional<std::uint64_t> split_size;
    if (!split_text.empty()) {
        try {
            std::size_t consumed = 0;
            constexpr auto megabyte = std::uint64_t{1024 * 1024};
            const auto megabytes = std::stoull(split_text, &consumed);
            if (consumed != split_text.size() || megabytes == 0 ||
                megabytes > std::numeric_limits<std::uint64_t>::max() / megabyte) {
                throw std::invalid_argument("invalid split size");
            }
            split_size = megabytes * megabyte;
        } catch (const std::exception&) {
            SetStatus(L"Split size must be a positive number of megabytes.", true);
            return;
        }
    }
    unfurl::CompressionOptions options;
    options.format = format_from_index(format_.SelectedIndex());
    options.password = password;
    options.split_size = split_size;
    SetBusy(true);
    SetStatus(L"Compressing…");
    std::thread([lifetime = get_strong(), paths, destination, name, options] {
        try {
            const auto result = unfurl::ArchiveEngine::compress(paths, destination, name, options);
            lifetime->dispatcher_.TryEnqueue([lifetime, output = result.output().wstring()] {
                lifetime->SetStatus(hstring(std::wstring(L"Created ") + output));
                lifetime->SetBusy(false);
            });
        } catch (const unfurl::ArchiveFailure& error) {
            lifetime->dispatcher_.TryEnqueue([lifetime, message = std::string(error.what())] {
                lifetime->SetStatus(from_utf8(message), true);
                lifetime->SetBusy(false);
            });
        }
    }).detach();
}

} // namespace winrt::Unfurl
