#include "MainPage.h"
#include "Localization.h"

#include <Windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace Windows::ApplicationModel::DataTransfer;
using namespace Windows::Storage;

namespace {

hstring from_utf8(std::string_view value) {
    return to_hstring(value);
}

hstring display_size(std::uint64_t bytes) {
    if (bytes < 1024)
        return to_hstring(bytes) + L" B";
    double amount = static_cast<double>(bytes);
    const wchar_t* units[] = {L"B", L"KiB", L"MiB", L"GiB", L"TiB"};
    int unit = 0;
    while (amount >= 1024 && unit < 4) {
        amount /= 1024;
        ++unit;
    }
    std::wostringstream text;
    text << std::fixed << std::setprecision(amount < 10 ? 1 : 0) << amount << L" " << units[unit];
    return hstring(text.str());
}

unfurl::ArchiveFormat format_from_index(std::int32_t index) {
    constexpr unfurl::ArchiveFormat formats[] = {unfurl::ArchiveFormat::zip,      unfurl::ArchiveFormat::seven_zip,
                                                 unfurl::ArchiveFormat::tar_gzip, unfurl::ArchiveFormat::tar_bzip2,
                                                 unfurl::ArchiveFormat::tar_xz,   unfurl::ArchiveFormat::tar_zstd};
    return formats[index >= 0 && index < 6 ? index : 0];
}

std::vector<std::filesystem::path> choose_paths(HWND owner, bool folders, bool archives, bool multiple = true) {
    com_ptr<IFileOpenDialog> dialog;
    check_hresult(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IFileOpenDialog),
                                   dialog.put_void()));
    FILEOPENDIALOGOPTIONS options{};
    check_hresult(dialog->GetOptions(&options));
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (multiple)
        options |= FOS_ALLOWMULTISELECT;
    options |= folders ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
    check_hresult(dialog->SetOptions(options));
    check_hresult(dialog->SetTitle(!multiple  ? L"选择保存位置"
                                   : folders  ? L"添加文件夹"
                                   : archives ? L"打开压缩包"
                                              : L"添加文件"));
    if (archives) {
        const COMDLG_FILTERSPEC filters[] = {
            {L"压缩包", L"*.zip;*.7z;*.rar;*.tar;*.gz;*.bz2;*.xz;*.zst;*.tgz;*.tbz2;*.txz;*.001"},
            {L"所有文件", L"*.*"}};
        check_hresult(dialog->SetFileTypes(2, filters));
    }
    const auto result = dialog->Show(owner);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return {};
    check_hresult(result);
    com_ptr<IShellItemArray> items;
    check_hresult(dialog->GetResults(items.put()));
    DWORD count{};
    check_hresult(items->GetCount(&count));
    std::vector<std::filesystem::path> paths;
    for (DWORD index = 0; index < count; ++index) {
        com_ptr<IShellItem> item;
        check_hresult(items->GetItemAt(index, item.put()));
        PWSTR path{};
        check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH, &path));
        paths.emplace_back(path);
        CoTaskMemFree(path);
    }
    return paths;
}

} // namespace

namespace winrt::Unfurl {

MainPage::MainPage() {
    dispatcher_ = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    BuildUi();
}

void MainPage::BuildUi() {
    Language(L"zh-CN");
    FontFamily(Media::FontFamily(L"Microsoft YaHei UI"));
    FontSize(14);
    // Let the window's Mica show through the NavigationView's translucent Fluent layers.
    Background(SolidColorBrush(Windows::UI::Color{0, 0, 0, 0}));
    const auto module = GetModuleHandleW(nullptr);
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(2), RT_RCDATA);
    if (!resource)
        throw_last_error();
    const auto data = LockResource(LoadResource(module, resource));
    const auto size = SizeofResource(module, resource);
    const auto root = Markup::XamlReader::Load(from_utf8({static_cast<const char*>(data), size})).as<Grid>();
    const auto find = [&root]<typename T>(const wchar_t* name, T& control) {
        control = root.FindName(name).as<T>();
        Automation::AutomationProperties::SetAutomationId(control, name);
    };
    find(L"WindowTitleBar", title_bar_);
    find(L"WorkspaceNavigation", navigation_);
    find(L"Workspace", workspace_);
    find(L"AppSettingsPage", app_settings_);
    find(L"ListHeader", list_header_);
    find(L"WorkspaceScroll", workspace_scroll_);
    find(L"BodyGrid", body_);
    find(L"WorkspaceHeading", workspace_heading_);
    find(L"CompressNavigation", compress_navigation_);
    find(L"ExtractNavigation", extract_navigation_);
    find(L"SelectAllItems", select_all_);
    find(L"ReadArchiveCommand", read_archive_);
    find(L"NameFormatGrid", name_format_);
    find(L"SplitRow", split_row_);
    find(L"ArchiveOptions", options_);
    find(L"EmptySelection", empty_selection_);
    find(L"EmptySelectionText", empty_selection_text_);
    find(L"DropZone", drop_zone_);
    find(L"ItemsPanel", items_panel_);
    find(L"SelectionTitle", selection_title_);
    find(L"SelectionDetail", selection_detail_);
    find(L"ItemsHeading", items_heading_);
    find(L"SettingsHeading", settings_heading_);
    find(L"StatusText", status_);
    find(L"FileList", items_);
    find(L"OperationProgress", progress_);
    find(L"FormatComboBox", format_);
    find(L"ThemeChoice", theme_);
    find(L"ArchivePasswordBox", password_);
    find(L"SplitSizeBox", split_size_);
    find(L"ArchiveNameBox", archive_name_);
    find(L"DestinationBox", destination_);
    find(L"DestinationPath", destination_path_);
    find(L"OpenArchiveCommand", open_);
    find(L"AddFilesCommand", add_files_);
    find(L"AddFolderCommand", add_folder_);
    find(L"ClearCommand", clear_);
    find(L"DestinationCommand", choose_destination_);
    find(L"RevealCommand", reveal_);
    find(L"ExtractCommand", extract_);
    find(L"CompressCommand", compress_);
    find(L"CancelCommand", cancel_);
    Automation::AutomationProperties::SetName(extract_, L"解压");
    Automation::AutomationProperties::SetName(compress_, L"压缩");
    Automation::AutomationProperties::SetName(reveal_, L"打开输出位置");
    Automation::AutomationProperties::SetName(open_, L"打开压缩包");
    Automation::AutomationProperties::SetName(add_files_, L"添加文件");

    title_bar_.PaneToggleRequested([this](auto&&, auto&&) { navigation_.IsPaneOpen(!navigation_.IsPaneOpen()); });
    options_.RegisterPropertyChangedCallback(Expander::IsExpandedProperty(),
                                             [this](auto&&, auto&&) { PrepareOptionsTransition(); });
    options_.SizeChanged([this](auto&&, SizeChangedEventArgs const& args) { UpdateOptionsViewport(args); });
    workspace_scroll_.ViewChanged([this](auto&&, ScrollViewerViewChangedEventArgs const& args) {
        if (collapse_scroll_target_ && !args.IsIntermediate()) {
            collapse_scroll_target_.reset();
            body_.MinHeight(0);
        }
    });
    navigation_.SelectionChanged([this](auto&&, NavigationViewSelectionChangedEventArgs const& args) {
        if (!args.SelectedItem())
            return;
        app_settings_.Visibility(args.IsSettingsSelected() ? Visibility::Visible : Visibility::Collapsed);
        workspace_.Visibility(args.IsSettingsSelected() ? Visibility::Collapsed : Visibility::Visible);
        if (args.IsSettingsSelected())
            return;
        const auto extracting = args.SelectedItem() == extract_navigation_;
        if (extract_mode_ != extracting) {
            extract_mode_ = extracting;
            ClearSelection();
        }
    });
    navigation_.Loaded([this](auto&&, auto&&) {
        const auto settings = navigation_.SettingsItem().as<NavigationViewItem>();
        settings.Content(box_value(L"设置"));
        settings.IsEnabled(!busy_);
        Automation::AutomationProperties::SetAutomationId(settings, L"SettingsNavigation");
        Automation::AutomationProperties::SetName(settings, L"设置");
        ToolTipService::SetToolTip(settings, box_value(L"设置"));
    });

    items_.SelectionChanged([this](auto&&, SelectionChangedEventArgs const& args) { OnEntrySelectionChanged(args); });
    items_.ContainerContentChanging([this](auto&&, ContainerContentChangingEventArgs const& args) {
        if (args.InRecycleQueue())
            return;
        const auto data =
            args.Item().as<Windows::Foundation::Collections::IMap<hstring, Windows::Foundation::IInspectable>>();
        Automation::AutomationProperties::SetName(args.ItemContainer(), unbox_value<hstring>(data.Lookup(L"Name")));
        Automation::AutomationProperties::SetAutomationId(
            args.ItemContainer(),
            (extract_mode_ ? hstring(L"ArchiveEntry") : hstring(L"SourceItem")) + to_hstring(args.ItemIndex()));
    });
    select_all_.Click([this](auto&&, auto&&) {
        if (busy_ || preview_entries_.empty())
            return;
        const auto all = std::ranges::all_of(entry_selection_, [](bool value) { return value; });
        updating_selection_ = true;
        if (all)
            items_.DeselectRange(Data::ItemIndexRange(0, items_.Items().Size()));
        else
            items_.SelectAll();
        std::fill(entry_selection_.begin(), entry_selection_.end(), !all);
        updating_selection_ = false;
        RefreshEntrySelection();
    });

    open_.Click([this](auto&&, auto&&) { PickFiles(true, false); });
    read_archive_.Click([this](auto&&, auto&&) { LoadArchivePreview(); });
    add_files_.Click([this](auto&&, auto&&) { PickFiles(false, false); });
    add_folder_.Click([this](auto&&, auto&&) { PickFiles(false, true); });
    clear_.Click([this](auto&&, auto&&) { ClearSelection(); });
    choose_destination_.Click([this](auto&&, auto&&) { PickDestination(); });
    reveal_.Click([this](auto&&, auto&&) { ShowOutput(); });
    extract_.Click(RoutedEventHandler{this, &MainPage::OnExtract});
    compress_.Click(RoutedEventHandler{this, &MainPage::OnCompress});
    cancel_.Click(RoutedEventHandler{this, &MainPage::OnCancel});
    format_.SelectionChanged(SelectionChangedEventHandler{this, &MainPage::OnFormatChanged});
    theme_.SelectionChanged([this](auto&&, auto&&) {
        RequestedTheme(theme_.SelectedIndex() == 1   ? ElementTheme::Light
                       : theme_.SelectedIndex() == 2 ? ElementTheme::Dark
                                                     : ElementTheme::Default);
    });
    ActualThemeChanged([this](auto&&, auto&&) {
        SetStatus(status_.Text(), status_error_, status_detail_);
        UpdateCaptionTheme();
    });
    drop_zone_.DragOver(DragEventHandler{this, &MainPage::HandleDragOver});
    drop_zone_.DragLeave([this](auto&&, auto&&) { drop_zone_.Opacity(1); });
    drop_zone_.Drop(DragEventHandler{this, &MainPage::HandleDrop});
    workspace_.SizeChanged(
        [this](auto&&, SizeChangedEventArgs const& args) { UpdateResponsiveLayout(args.NewSize().Width); });
    workspace_.Loaded([this](auto&&, auto&&) { (extract_mode_ ? open_ : add_files_).Focus(FocusState::Pointer); });
    Content(root);
    navigation_.SelectedItem(compress_navigation_);
    RefreshSelection();
    SetBusy(false);
}

void MainPage::UpdateResponsiveLayout(double width) {
    const auto compact = width < 560;
    if (compact_layout_ != compact) {
        compact_layout_ = compact;
        name_format_.RowSpacing(compact ? 10 : 0);
        Grid::SetColumn(archive_name_, compact ? 1 : 2);
        Grid::SetRow(archive_name_, compact ? 1 : 0);
        Grid::SetColumnSpan(archive_name_, compact ? 3 : 1);
    }
    items_.Height(std::clamp(static_cast<double>(items_.Items().Size()) * 32 + 8, 40.0, 136.0));
}

void MainPage::InitializeWindow(Window const& window, std::uintptr_t handle) {
    window_handle_ = handle;
    title_bar_chrome_ = window.AppWindow().TitleBar();
    window.SetTitleBar(title_bar_);
    const auto transparent = Windows::UI::Color{0, 0, 0, 0};
    title_bar_chrome_.ButtonBackgroundColor(transparent);
    title_bar_chrome_.ButtonInactiveBackgroundColor(transparent);
    UpdateCaptionTheme();
}

void MainPage::UpdateCaptionTheme() {
    if (!title_bar_chrome_)
        return;
    title_bar_chrome_.PreferredTheme(ActualTheme() == ElementTheme::Dark
                                         ? Microsoft::UI::Windowing::TitleBarTheme::Dark
                                         : Microsoft::UI::Windowing::TitleBarTheme::Light);
    const auto dark = ActualTheme() == ElementTheme::Dark;
    const auto foreground = dark ? Windows::UI::Color{255, 255, 255, 255} : Windows::UI::Color{255, 26, 26, 26};
    title_bar_chrome_.ButtonForegroundColor(foreground);
    title_bar_chrome_.ButtonHoverForegroundColor(foreground);
    title_bar_chrome_.ButtonPressedForegroundColor(foreground);
    title_bar_chrome_.ButtonInactiveForegroundColor(dark ? Windows::UI::Color{255, 153, 153, 153}
                                                         : Windows::UI::Color{255, 110, 110, 110});
    title_bar_chrome_.ButtonHoverBackgroundColor(dark ? Windows::UI::Color{18, 255, 255, 255}
                                                      : Windows::UI::Color{9, 0, 0, 0});
    title_bar_chrome_.ButtonPressedBackgroundColor(dark ? Windows::UI::Color{12, 255, 255, 255}
                                                        : Windows::UI::Color{15, 0, 0, 0});
}

void MainPage::PrepareOptionsTransition() {
    if (collapse_scroll_target_)
        workspace_scroll_.ChangeView(nullptr, workspace_scroll_.VerticalOffset(), nullptr, true);
    collapse_scroll_target_.reset();
    reveal_options_ = options_.IsExpanded();
    if (reveal_options_) {
        collapsing_options_height_ = 0;
        body_.MinHeight(0);
    } else {
        // Retain the old scroll extent until the animated return reaches a valid offset.
        collapsing_options_height_ = options_.ActualHeight();
        body_.MinHeight(body_.ActualHeight());
    }
}

void MainPage::UpdateOptionsViewport(SizeChangedEventArgs const& args) {
    if (reveal_options_ && options_.IsExpanded() && args.NewSize().Height > args.PreviousSize().Height) {
        reveal_options_ = false;
        BringIntoViewOptions request;
        request.AnimationDesired(Windows::UI::ViewManagement::UISettings().AnimationsEnabled());
        options_.StartBringIntoView(request);
    } else if (!options_.IsExpanded() && collapsing_options_height_ > args.NewSize().Height) {
        const auto content_height = body_.MinHeight() - (collapsing_options_height_ - args.NewSize().Height);
        collapsing_options_height_ = 0;
        const auto target = std::min(workspace_scroll_.VerticalOffset(),
                                     std::max(0.0, content_height - workspace_scroll_.ViewportHeight()));
        collapse_scroll_target_ = target;
        if (std::abs(workspace_scroll_.VerticalOffset() - target) < 0.5 ||
            !workspace_scroll_.ChangeView(nullptr, target, nullptr,
                                          !Windows::UI::ViewManagement::UISettings().AnimationsEnabled())) {
            collapse_scroll_target_.reset();
            body_.MinHeight(0);
        }
    }
}

bool MainPage::Busy() const {
    return busy_;
}

void MainPage::RequestClose(std::function<void()> close) {
    close_when_idle_ = std::move(close);
    if (busy_)
        OnCancel(nullptr, nullptr);
    else if (close_when_idle_)
        std::exchange(close_when_idle_, {})();
}

void MainPage::SetStatus(hstring const& text, bool error, hstring const& detail) {
    status_error_ = error;
    status_.Text(text);
    status_detail_ = detail.empty() ? text : detail;
    ToolTipService::SetToolTip(status_, box_value(status_detail_));
    if (error) {
        status_.Foreground(SolidColorBrush(ActualTheme() == ElementTheme::Dark ? Windows::UI::Color{255, 255, 153, 164}
                                                                               : Windows::UI::Color{255, 196, 43, 28}));
        status_.Opacity(1);
    } else {
        status_.ClearValue(TextBlock::ForegroundProperty());
        status_.Opacity(0.75);
    }
}

void MainPage::SetFailure(std::string const& message, std::optional<unfurl::ArchiveFailure::Code> code) {
    const auto translated = hstring(unfurl::localization::error_message(message, code));
    SetStatus(translated, true, translated + L"\n\n原始错误：" + from_utf8(message));
}

void MainPage::SetBusy(bool busy) {
    busy_ = busy;
    const auto extracting = extract_mode_;
    progress_.IsIndeterminate(busy);
    progress_.Opacity(busy ? 1 : 0);
    open_.IsEnabled(!busy);
    add_files_.IsEnabled(!busy);
    add_folder_.IsEnabled(!busy);
    clear_.IsEnabled(!busy && (!selected_archives_.empty() || !selected_paths_.empty()));
    compress_navigation_.IsEnabled(!busy);
    extract_navigation_.IsEnabled(!busy);
    if (const auto settings = navigation_.SettingsItem().try_as<NavigationViewItem>())
        settings.IsEnabled(!busy);
    theme_.IsEnabled(!busy);
    items_.IsEnabled(!busy);
    select_all_.IsEnabled(!busy && !preview_entries_.empty());
    read_archive_.IsEnabled(!busy);
    archive_name_.IsEnabled(!busy);
    format_.IsEnabled(!busy);
    password_.IsEnabled(!busy && (extracting || format_.SelectedIndex() == 0));
    split_size_.IsEnabled(!busy && !extracting && format_.SelectedIndex() == 0);
    choose_destination_.IsEnabled(!busy);
    drop_zone_.IsHitTestVisible(!busy);
    extract_.Visibility(extracting && !busy ? Visibility::Visible : Visibility::Collapsed);
    compress_.Visibility(!extracting && !busy ? Visibility::Visible : Visibility::Collapsed);
    extract_.IsEnabled(!busy && extracting && std::ranges::any_of(entry_selection_, [](bool value) { return value; }));
    compress_.IsEnabled(!busy && !selected_paths_.empty());
    cancel_.Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
    cancel_.IsEnabled(busy && cancellation_ && !cancellation_->load(std::memory_order_relaxed));
    if (!busy && close_when_idle_)
        std::exchange(close_when_idle_, {})();
}

void MainPage::OnFormatChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&) {
    const auto zip = format_.SelectedIndex() == 0;
    password_.PlaceholderText(extract_mode_ ? L"加密压缩包需要密码" : zip ? L"留空则不加密" : L"仅支持 ZIP");
    split_size_.PlaceholderText(zip ? L"不分卷" : L"仅支持 ZIP");
    SetBusy(busy_);
}

void MainPage::OnCancel(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (!cancellation_ || cancellation_->exchange(true, std::memory_order_relaxed))
        return;
    cancel_.IsEnabled(false);
    SetStatus(L"正在取消...");
}

void MainPage::PickFiles(bool archives, bool folders) {
    if (busy_)
        return;
    try {
        auto paths = choose_paths(reinterpret_cast<HWND>(window_handle_), folders, archives);
        if (paths.empty())
            return;
        const auto extending = !archives && !selected_paths_.empty();
        const auto name = archive_name_.Text();
        const auto password = password_.Password();
        if (!archives)
            paths.insert(paths.begin(), selected_paths_.begin(), selected_paths_.end());
        LoadPaths(std::move(paths), !archives);
        if (extending) {
            archive_name_.Text(name);
            password_.Password(password);
        }
    } catch (const hresult_error& error) {
        SetFailure(to_string(error.message()));
    } catch (const std::exception& error) {
        SetFailure(error.what());
    }
}

void MainPage::PickDestination() {
    if (busy_)
        return;
    try {
        auto paths = choose_paths(reinterpret_cast<HWND>(window_handle_), true, false, false);
        if (paths.empty())
            return;
        output_directory_ = paths.front();
        RefreshDestination();
    } catch (const hresult_error& error) {
        SetFailure(to_string(error.message()));
    }
}

void MainPage::ShowOutput() {
    if (last_output_.empty())
        return;
    PIDLIST_ABSOLUTE item{};
    const auto result = SHParseDisplayName(last_output_.c_str(), nullptr, &item, 0, nullptr);
    if (FAILED(result)) {
        SetStatus(L"输出文件或文件夹已不可用。", true);
        return;
    }
    const auto opened = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
    CoTaskMemFree(item);
    if (FAILED(opened))
        SetStatus(L"文件资源管理器无法打开输出位置。", true);
}

void MainPage::ClearSelection() {
    if (busy_)
        return;
    ++operation_id_;
    selected_paths_.clear();
    selected_archives_.clear();
    std::vector<PreviewEntry>().swap(preview_entries_);
    std::vector<bool>().swap(entry_selection_);
    output_directory_.clear();
    last_output_.clear();
    updating_selection_ = true;
    items_.Items().Clear();
    updating_selection_ = false;
    read_archive_.Visibility(Visibility::Collapsed);
    password_.Password(L"");
    archive_name_.Text(L"");
    options_.IsExpanded(false);
    reveal_.Visibility(Visibility::Collapsed);
    RefreshSelection();
    SetStatus(L"就绪");
    SetBusy(false);
}

void MainPage::RefreshSelection() {
    const auto extracting = extract_mode_;
    const auto& paths = extracting ? selected_archives_ : selected_paths_;
    items_.SelectionMode(extracting ? ListViewSelectionMode::Multiple : ListViewSelectionMode::None);
    select_all_.Visibility(extracting ? Visibility::Visible : Visibility::Collapsed);
    list_header_.ColumnDefinitions().GetAt(0).Width(GridLength{extracting ? 52.0 : 20.0, GridUnitType::Pixel});
    drop_zone_.MinHeight(paths.empty() ? 104 : 0);
    selection_title_.Text(to_hstring(paths.size()) + (extracting ? L" 个压缩包" : L" 项"));
    selection_detail_.Text(paths.empty()       ? L""
                           : paths.size() == 1 ? hstring(paths.front().filename().wstring())
                                               : to_hstring(paths.size()) + L" 个压缩包");
    selection_detail_.Visibility(extracting && !paths.empty() ? Visibility::Visible : Visibility::Collapsed);
    const auto source_path = paths.empty() ? hstring{} : hstring(paths.front().wstring());
    ToolTipService::SetToolTip(selection_title_, box_value(source_path));
    ToolTipService::SetToolTip(selection_detail_, box_value(source_path));
    items_heading_.Text(extracting ? L"压缩包内容" : L"待压缩项目");
    items_panel_.Visibility(paths.empty() ? Visibility::Collapsed : Visibility::Visible);
    empty_selection_.Visibility(paths.empty() ? Visibility::Visible : Visibility::Collapsed);
    empty_selection_text_.Text(extracting ? L"尚未选择压缩包" : L"尚未选择文件或文件夹");
    workspace_heading_.Text(extracting ? L"解压压缩包" : L"创建压缩包");
    settings_heading_.Text(L"输出设置");
    open_.Visibility(extracting ? Visibility::Visible : Visibility::Collapsed);
    add_files_.Visibility(extracting ? Visibility::Collapsed : Visibility::Visible);
    add_folder_.Visibility(extracting ? Visibility::Collapsed : Visibility::Visible);
    name_format_.Visibility(extracting ? Visibility::Collapsed : Visibility::Visible);
    split_row_.Visibility(extracting ? Visibility::Collapsed : Visibility::Visible);
    UpdateResponsiveLayout(workspace_.ActualWidth());
    password_.PlaceholderText(extracting                     ? L"加密压缩包需要密码"
                              : format_.SelectedIndex() == 0 ? L"留空则不加密"
                                                             : L"仅支持 ZIP");
    RefreshDestination();
    if (extracting)
        RefreshEntrySelection();
}

void MainPage::RefreshEntrySelection() {
    const auto count = std::ranges::count(entry_selection_, true);
    const auto total = preview_entries_.size();
    selection_title_.Text(L"已选 " + to_hstring(count) + L" / " + to_hstring(total) + L" 项");
    select_all_.IsChecked(count == 0                                 ? Windows::Foundation::IReference<bool>(false)
                          : static_cast<std::size_t>(count) == total ? Windows::Foundation::IReference<bool>(true)
                                                                     : nullptr);
    select_all_.IsEnabled(!busy_ && total > 0);
    extract_.IsEnabled(!busy_ && count > 0);
}

void MainPage::OnEntrySelectionChanged(SelectionChangedEventArgs const& args) {
    if (updating_selection_ || !extract_mode_ || preview_entries_.empty())
        return;
    const auto index_of = [](auto const& value) {
        return unbox_value<std::uint32_t>(
            value.template as<Windows::Foundation::Collections::IMap<hstring, Windows::Foundation::IInspectable>>()
                .Lookup(L"Index"));
    };
    for (const auto& value : args.AddedItems())
        entry_selection_.at(index_of(value)) = true;
    for (const auto& value : args.RemovedItems())
        entry_selection_.at(index_of(value)) = false;
    const auto native_selection = entry_selection_;
    const auto descendants = [this](std::size_t parent, auto action) {
        const auto& folder = preview_entries_[parent];
        const auto prefix = folder.item.path + "/";
        for (std::size_t i = 0; i < preview_entries_.size(); ++i) {
            const auto& entry = preview_entries_[i];
            if (entry.archive == folder.archive && entry.item.path.starts_with(prefix))
                action(i);
        }
    };
    if (items_.SelectedItems().Size() != preview_entries_.size()) {
        for (const auto& value : args.RemovedItems()) {
            const auto index = index_of(value);
            if (preview_entries_[index].item.directory)
                descendants(index, [this](auto child) { entry_selection_[child] = false; });
        }
        for (const auto& value : args.AddedItems()) {
            const auto index = index_of(value);
            entry_selection_[index] = true;
            if (preview_entries_[index].item.directory)
                descendants(index, [this](auto child) { entry_selection_[child] = true; });
        }
        // A checked folder always represents its entire subtree, even after individual children change.
        for (std::size_t i = preview_entries_.size(); i-- > 0;) {
            if (!preview_entries_[i].item.directory)
                continue;
            bool has_children = false;
            bool all = true;
            descendants(i, [&](auto child) {
                has_children = true;
                all = all && entry_selection_[child];
            });
            if (has_children)
                entry_selection_[i] = all;
        }
    }
    updating_selection_ = true;
    for (std::size_t i = 0; i < entry_selection_.size();) {
        if (entry_selection_[i] == native_selection[i]) {
            ++i;
            continue;
        }
        const auto first = i;
        const auto selected = entry_selection_[i];
        while (i < entry_selection_.size() && entry_selection_[i] == selected &&
               entry_selection_[i] != native_selection[i])
            ++i;
        const Data::ItemIndexRange range(static_cast<std::int32_t>(first), static_cast<std::uint32_t>(i - first));
        if (selected)
            items_.SelectRange(range);
        else
            items_.DeselectRange(range);
    }
    updating_selection_ = false;
    RefreshEntrySelection();
}

void MainPage::RefreshDestination() {
    const auto& paths = extract_mode_ ? selected_archives_ : selected_paths_;
    auto destination = output_directory_;
    if (destination.empty() && !paths.empty() && !(extract_mode_ && paths.size() > 1))
        destination = paths.front().parent_path();
    destination_.Text(destination.empty()
                          ? (extract_mode_ && paths.size() > 1 ? L"各压缩包所在文件夹" : L"源文件所在文件夹")
                          : hstring((destination.filename().empty() ? destination : destination.filename()).wstring()));
    destination_path_.Text(hstring(destination.wstring()));
    destination_path_.Visibility(destination.empty() ? Visibility::Collapsed : Visibility::Visible);
    ToolTipService::SetToolTip(choose_destination_,
                               box_value(destination.empty() ? destination_.Text() : destination_path_.Text()));
}

void MainPage::AddRow(hstring const& path, std::uint64_t size, bool directory) {
    Windows::Foundation::Collections::PropertySet row;
    row.Insert(L"Name", box_value(path));
    row.Insert(L"Glyph", box_value(directory ? L"\uE8B7" : L"\uE8A5"));
    row.Insert(L"Size", box_value(directory ? hstring(L"文件夹") : display_size(size)));
    row.Insert(L"Index", box_value(items_.Items().Size()));
    items_.Items().Append(row);
}

void MainPage::HandleDragOver(Windows::Foundation::IInspectable const&, DragEventArgs const& args) {
    if (busy_ || !args.DataView().Contains(StandardDataFormats::StorageItems())) {
        args.AcceptedOperation(DataPackageOperation::None);
        return;
    }
    args.AcceptedOperation(DataPackageOperation::Copy);
    drop_zone_.Opacity(0.7);
    args.Handled(true);
}

fire_and_forget MainPage::HandleDrop(Windows::Foundation::IInspectable const&, DragEventArgs const& args) {
    auto lifetime = get_strong();
    const auto deferral = args.GetDeferral();
    drop_zone_.Opacity(1);
    try {
        if (!busy_ && args.DataView().Contains(StandardDataFormats::StorageItems())) {
            const auto items = co_await args.DataView().GetStorageItemsAsync();
            std::vector<std::filesystem::path> paths;
            for (const auto& item : items)
                paths.emplace_back(item.Path().c_str());
            LoadPaths(std::move(paths));
        }
    } catch (const hresult_error& error) {
        SetFailure(to_string(error.message()));
    } catch (const std::exception& error) {
        SetFailure(error.what());
    }
    deferral.Complete();
}

void MainPage::LoadPaths(std::vector<std::filesystem::path> paths, bool compress) {
    if (paths.empty() || busy_)
        return;
    try {
        std::vector<std::filesystem::path> unique;
        for (const auto& path : paths) {
            auto absolute = std::filesystem::absolute(path).lexically_normal();
            if (!std::filesystem::exists(absolute))
                throw std::runtime_error("A selected file or folder no longer exists.");
            if (std::ranges::find(unique, absolute) == unique.end())
                unique.push_back(std::move(absolute));
        }
        paths = std::move(unique);
        const auto extracting = !compress && std::ranges::all_of(paths, unfurl::ArchiveEngine::is_archive);
        extract_mode_ = extracting;
        navigation_.SelectedItem(extracting ? extract_navigation_ : compress_navigation_);
        selected_paths_ = extracting ? std::vector<std::filesystem::path>{} : paths;
        selected_archives_ = extracting ? paths : std::vector<std::filesystem::path>{};
        preview_entries_.clear();
        entry_selection_.clear();
        updating_selection_ = true;
        items_.Items().Clear();
        updating_selection_ = false;
        read_archive_.Visibility(Visibility::Collapsed);
        last_output_.clear();
        reveal_.Visibility(Visibility::Collapsed);
        password_.Password(L"");
        if (!extracting) {
            const auto name =
                paths.size() == 1
                    ? (std::filesystem::is_directory(paths.front()) ? paths.front().filename() : paths.front().stem())
                    : std::filesystem::path(L"压缩包");
            archive_name_.Text(hstring(name.wstring()));
        }
        RefreshSelection();
        if (!extracting) {
            for (std::size_t i = 0; i < std::min(paths.size(), unfurl::ArchiveEngine::preview_limit); ++i) {
                const auto folder = std::filesystem::is_directory(paths[i]);
                AddRow(hstring(paths[i].filename().wstring()), folder ? 0 : std::filesystem::file_size(paths[i]),
                       folder);
            }
            UpdateResponsiveLayout(workspace_.ActualWidth());
            SetStatus(L"已选择 " + to_hstring(paths.size()) + L" 项，可以开始压缩");
            SetBusy(false);
            return;
        }
        LoadArchivePreview();
    } catch (const std::exception& error) {
        SetFailure(error.what());
        SetBusy(false);
    }
}

void MainPage::LoadArchivePreview() {
    if (busy_ || selected_archives_.empty())
        return;
    preview_entries_.clear();
    entry_selection_.clear();
    updating_selection_ = true;
    items_.Items().Clear();
    updating_selection_ = false;
    RefreshEntrySelection();
    const auto operation = ++operation_id_;
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = cancellation_;
    read_archive_.Visibility(Visibility::Visible);
    SetStatus(L"正在读取压缩包...");
    SetBusy(true);
    std::thread([lifetime = get_strong(), sources = selected_archives_, password = to_string(password_.Password()),
                 cancellation, operation] {
        try {
            std::vector<PreviewEntry> entries;
            bool encrypted = false;
            for (std::size_t i = 0; i < sources.size(); ++i) {
                auto preview = unfurl::ArchiveEngine::preview(
                    sources[i], password.empty() ? std::nullopt : std::optional<std::string_view>(password),
                    std::numeric_limits<std::uint32_t>::max(),
                    [cancellation] { return cancellation->load(std::memory_order_relaxed); });
                encrypted = encrypted || preview.encrypted;
                std::ranges::sort(preview.items, {}, &unfurl::ArchiveItem::path);
                for (auto& item : preview.items) {
                    if (!item.path.empty())
                        entries.push_back({std::move(item), i});
                }
            }
            lifetime->dispatcher_.TryEnqueue([lifetime, entries = std::move(entries), encrypted, operation,
                                              cancellation]() mutable {
                if (lifetime->operation_id_ != operation)
                    return;
                if (cancellation->load(std::memory_order_relaxed)) {
                    lifetime->SetStatus(L"已取消");
                    lifetime->SetBusy(false);
                    return;
                }
                lifetime->preview_entries_ = std::move(entries);
                lifetime->updating_selection_ = true;
                for (const auto& entry : lifetime->preview_entries_) {
                    const auto name = lifetime->selected_archives_.size() > 1
                                          ? hstring(lifetime->selected_archives_[entry.archive].filename().wstring()) +
                                                L" / " + from_utf8(entry.item.path)
                                          : from_utf8(entry.item.path);
                    lifetime->AddRow(name, entry.item.size, entry.item.directory);
                }
                lifetime->items_.SelectAll();
                lifetime->entry_selection_.assign(lifetime->preview_entries_.size(), true);
                lifetime->updating_selection_ = false;
                lifetime->read_archive_.Visibility(Visibility::Collapsed);
                lifetime->options_.IsExpanded(encrypted);
                lifetime->UpdateResponsiveLayout(lifetime->workspace_.ActualWidth());
                lifetime->RefreshEntrySelection();
                lifetime->SetStatus(lifetime->preview_entries_.empty() ? L"压缩包为空"
                                    : encrypted                        ? L"已加密；可以开始解压"
                                                                       : L"可以开始解压");
                lifetime->SetBusy(false);
            });
        } catch (const unfurl::ArchiveFailure& error) {
            if (error.code() == unfurl::ArchiveFailure::Code::password_required) {
                lifetime->dispatcher_.TryEnqueue([lifetime, operation] {
                    if (lifetime->operation_id_ == operation)
                        lifetime->options_.IsExpanded(true);
                });
            }
            lifetime->FinishFailure(error.what(), error.code() == unfurl::ArchiveFailure::Code::cancelled, operation,
                                    error.code());
        } catch (const std::exception& error) {
            lifetime->FinishFailure(error.what(), false, operation);
        }
    }).detach();
}

void MainPage::FinishFailure(std::string message, bool cancelled, std::uint64_t operation,
                             std::optional<unfurl::ArchiveFailure::Code> code) {
    dispatcher_.TryEnqueue([lifetime = get_strong(), message = std::move(message), cancelled, operation, code] {
        if (lifetime->operation_id_ != operation)
            return;
        if (cancelled)
            lifetime->SetStatus(L"已取消");
        else
            lifetime->SetFailure(message, code);
        lifetime->SetBusy(false);
    });
}

void MainPage::StartArchiveTask(ArchiveTask task, bool extracting) {
    const auto operation = ++operation_id_;
    cancellation_ = std::make_shared<std::atomic_bool>(false);
    const auto cancellation = cancellation_;
    reveal_.Visibility(Visibility::Collapsed);
    SetStatus(extracting ? L"正在解压..." : L"正在压缩...");
    SetBusy(true);
    std::thread([lifetime = get_strong(), task = std::move(task), extracting, cancellation, operation] {
        try {
            auto last_progress = std::chrono::steady_clock::now() - std::chrono::seconds(1);
            const auto progress_pending = std::make_shared<std::atomic_bool>(false);
            const auto result = task(
                [cancellation] { return cancellation->load(std::memory_order_relaxed); },
                [lifetime, cancellation, progress_pending, operation, extracting,
                 &last_progress](const unfurl::ArchiveUpdate& update) {
                    const auto now = std::chrono::steady_clock::now();
                    if (cancellation->load(std::memory_order_relaxed) ||
                        now - last_progress < std::chrono::milliseconds(100) ||
                        progress_pending->exchange(true, std::memory_order_relaxed))
                        return;
                    last_progress = now;
                    if (!lifetime->dispatcher_.TryEnqueue([lifetime, cancellation, progress_pending, operation,
                                                           extracting, path = update.path, bytes = update.bytes] {
                            progress_pending->store(false, std::memory_order_relaxed);
                            if (lifetime->operation_id_ != operation || cancellation->load(std::memory_order_relaxed))
                                return;
                            lifetime->SetStatus((extracting ? hstring(L"正在解压：") : hstring(L"正在压缩：")) +
                                                from_utf8(path) + L"；" + display_size(bytes));
                        }))
                        progress_pending->store(false, std::memory_order_relaxed);
                });
            lifetime->dispatcher_.TryEnqueue([lifetime, result, extracting, operation] {
                if (lifetime->operation_id_ != operation)
                    return;
                lifetime->last_output_ = result.output();
                lifetime->reveal_.Visibility(Visibility::Visible);
                const auto output = extracting && result.outputs.size() > 1
                                        ? to_hstring(result.outputs.size()) + L" 个压缩包"
                                        : hstring(result.output().filename().wstring());
                const auto suffix = !extracting && result.outputs.size() > 1
                                        ? L"；共 " + to_hstring(result.outputs.size()) + L" 个分卷"
                                        : L"；共 " + to_hstring(result.entries) + L" 项";
                lifetime->SetStatus((extracting ? hstring(L"已解压：") : hstring(L"已创建：")) + output + suffix);
                lifetime->SetBusy(false);
            });
        } catch (const unfurl::ArchiveFailure& error) {
            lifetime->FinishFailure(error.what(), error.code() == unfurl::ArchiveFailure::Code::cancelled, operation,
                                    error.code());
        } catch (const std::exception& error) {
            lifetime->FinishFailure(error.what(), false, operation);
        }
    }).detach();
}

void MainPage::OnExtract(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (busy_ || !std::ranges::any_of(entry_selection_, [](bool value) { return value; }))
        return;
    std::vector<std::vector<std::string>> selections(selected_archives_.size());
    std::vector<std::size_t> counts(selected_archives_.size());
    for (std::size_t i = 0; i < preview_entries_.size(); ++i) {
        const auto& entry = preview_entries_[i];
        ++counts[entry.archive];
        if (entry_selection_[i])
            selections[entry.archive].push_back(entry.item.path);
    }
    StartArchiveTask(
        [sources = selected_archives_, destination = output_directory_, selections = std::move(selections),
         counts = std::move(counts),
         password = to_string(password_.Password())](const auto& cancelled, const auto& progress) {
            unfurl::ArchiveResult total;
            for (std::size_t i = 0; i < sources.size(); ++i) {
                if (selections[i].empty())
                    continue;
                const auto& source = sources[i];
                const auto selection = selections[i].size() == counts[i]
                                           ? std::nullopt
                                           : std::optional<std::vector<std::string>>(selections[i]);
                const auto result = unfurl::ArchiveEngine::extract(
                    source, destination.empty() ? source.parent_path() : destination,
                    password.empty() ? std::nullopt : std::optional<std::string_view>(password), cancelled, progress,
                    selection);
                total.outputs.insert(total.outputs.end(), result.outputs.begin(), result.outputs.end());
                total.entries += result.entries;
                total.bytes += result.bytes;
            }
            return total;
        },
        true);
}

void MainPage::OnCompress(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) {
    if (busy_ || selected_paths_.empty())
        return;
    unfurl::CompressionOptions options;
    options.format = format_from_index(format_.SelectedIndex());
    if (options.format == unfurl::ArchiveFormat::zip) {
        options.password = to_string(password_.Password());
        const auto text = to_string(split_size_.Text());
        if (!text.empty()) {
            std::uint64_t megabytes{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), megabytes);
            constexpr auto megabyte = std::uint64_t{1024 * 1024};
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || megabytes == 0 ||
                megabytes > std::numeric_limits<std::uint64_t>::max() / megabyte) {
                SetStatus(L"分卷大小必须为正整数，单位为 MiB。", true);
                split_size_.Focus(FocusState::Programmatic);
                return;
            }
            options.split_size = megabytes * megabyte;
        }
    }
    const auto destination = output_directory_.empty() ? selected_paths_.front().parent_path() : output_directory_;
    StartArchiveTask(
        [sources = selected_paths_, destination,
         name = to_string(archive_name_.Text().empty() ? hstring(L"压缩包") : archive_name_.Text()),
         options](const auto& cancelled, const auto& progress) {
            return unfurl::ArchiveEngine::compress(sources, destination, name, options, cancelled, progress);
        },
        false);
}

} // namespace winrt::Unfurl
